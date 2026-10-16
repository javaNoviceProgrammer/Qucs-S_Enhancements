/*
 * fem_materials.cpp - the multiphysics solver's library of materials
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Sources. The formulas are their authors', and so are the figures where
 * nothing else is said:
 * - III-V band parameters: I. Vurgaftman, J. R. Meyer, L. R. Ram-Mohan,
 *   J. Appl. Phys. 89, 5815 (2001); the nitrides': I. Vurgaftman, J. R.
 *   Meyer, J. Appl. Phys. 94, 3675 (2003).
 * - Gaps of T: Y. P. Varshni, Physica 34, 149 (1967).
 * - Mobility by doping: D. M. Caughey, R. E. Thomas, Proc. IEEE 55, 2192
 *   (1967); G. Masetti, M. Severi, S. Solmi, IEEE Trans. Electron Devices
 *   30, 764 (1983).
 * - Band gap narrowing: J. W. Slotboom, H. C. de Graaff, Solid-State
 *   Electron. 19, 857 (1976); D. B. M. Klaassen, J. W. Slotboom, H. C. de
 *   Graaff, Solid-State Electron. 35, 125 (1992).
 * - Lifetimes by doping: J. G. Fossum, D. S. Lee, Solid-State Electron.
 *   25, 741 (1982). Trap-assisted and band-to-band tunnelling: G. A. M.
 *   Hurkx, D. B. M. Klaassen, M. P. G. Knuvers, IEEE Trans. Electron
 *   Devices 39, 331 (1992).
 * - Impact ionization: R. van Overstraeten, H. de Man, Solid-State
 *   Electron. 13, 583 (1970); S. Selberherr, Analysis and Simulation of
 *   Semiconductor Devices (Springer, 1984).
 * - Conductivity of heat of T: V. Palankovski, R. Quay, Analysis and
 *   Simulation of Heterostructure Devices (Springer, 2004).
 * - Gate dielectrics' permittivities: J. Robertson, Eur. Phys. J. Appl.
 *   Phys. 28, 265 (2004).
 * - Indices of the wavelength, each checked against the measured spectra
 *   of the refractiveindex.info database (M. N. Polyanskiy, Sci. Data 11,
 *   94, 2024; CC0) - the fits ours, made to them: silicon H. H. Li, J. Phys.
 *   Chem. Ref. Data 9, 561 (1980) and M. A. Green, Sol. Energy Mater. Sol.
 *   Cells 92, 1305 (2008) - its n fitted, its absorption as measured;
 *   germanium T. N. Nunley et al., J. Vac. Sci. Technol. B 34, 061205
 *   (2016) and J. H. Burnett et al., Proc. SPIE 9974, 99740X (2016); the
 *   III-Vs M. A. Afromowitz's oscillator (Solid State Commun. 15, 59, 1974)
 *   with a lattice term, fitted to T. Skauli et al.'s GaAs (J. Appl. Phys.
 *   94, 6447, 2003), G. D. Pettit and W. J. Turner's InP (J. Appl. Phys.
 *   36, 2081, 1965), O. G. Lorimor and W. G. Spitzer's InAs, R. E. Fern and
 *   A. Onton's AlAs, W. L. Bond's GaP, S. Zollner's and S. Adachi's AlSb,
 *   A. B. Djurišić's and Adachi's GaSb, Adachi's InSb; their gaps'
 *   absorption to D. E. Aspnes and A. A. Studna's (Phys. Rev. B 27, 985,
 *   1983) GaAs and InP, G. E. Jellison's GaP; SiO2 I. H. Malitson, J. Opt.
 *   Soc. Am. 55, 1205 (1965), its infrared absorption D. Franta et al.'s
 *   (Proc. SPIE 9890, 989014, 2016);
 *   Si3N4 K. Luke et al., Opt. Lett. 40, 4823 (2015); sapphire I. H.
 *   Malitson, M. J. Dodge, J. Opt. Soc. Am. 62, 1405 (1972); TiO2 J. R.
 *   DeVore, J. Opt. Soc. Am. 41, 416 (1951); ZrO2 D. L. Wood, K. Nassau,
 *   Appl. Opt. 21, 2978 (1982); HfO2 M. F. Al-Kuhaili, Opt. Mater. 27, 383
 *   (2004); the films' L. V. Rodriguez de Marcos et al.'s (ALD alumina),
 *   T. J. Bright et al.'s (sputtered hafnia), L. Yu. Beliaev et al.'s
 *   (PECVD nitride); CdTe D. T. F. Marple, CdSe M. P. Lisitsa et al., CdS
 *   T. M. Bieniewski and S. J. Czyzak, GaN A. S. Barker and M. Ilegems;
 *   LiTaO3 W. L. Bond's points; water M. Daimon and A. Masumura, Appl. Opt.
 *   46, 3811 (2007) and G. M. Hale and M. R. Querry, Appl. Opt. 12, 555
 *   (1973); gold A. Vial et al., Phys. Rev. B 71, 085416 (2005); copper and
 *   aluminium A. D. Rakić et al., Appl. Opt. 37, 5271 (1998); silver H. U.
 *   Yang et al., Phys. Rev. B 91, 235137 (2015).
 * Read from the Lumerical material database (Ansys's: its figures, not its
 * file; it cites no sources): the model sets' figures of the
 * semiconductors but silicon (ModelSet: the models above, their parameters
 * there), the II-VIs' masses and χ (from their work functions), the gate
 * dielectrics' and AlP's heat, SiGe's X gap bowing and alloy scattering,
 * the temperature exponents of silicon's models. Silicon's model figures
 * are the semiconductor features' own defaults.
 */
#include "fem_materials.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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

const QStringList BasicKeys = {QStringLiteral("epsilonr"), QStringLiteral("tand"), QStringLiteral("sigma"),
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
                                  QStringLiteral("bands"),    QStringLiteral("lambdag"), QStringLiteral("phi"),
                                  QStringLiteral("composition"), QStringLiteral("lattice"),
                                  QStringLiteral("nrange"),   QStringLiteral("sigmaFCn"), QStringLiteral("sigmaFCp"),
                                  // (A crystal's: its orientation and tensors.)
                                  QStringLiteral("orientation"), QStringLiteral("cut"), QStringLiteral("along"),
                                  QStringLiteral("euler_phi"), QStringLiteral("euler_theta"), QStringLiteral("euler_psi"),
                                  QStringLiteral("C"),        QStringLiteral("e"),    QStringLiteral("d"),
                                  QStringLiteral("r"),        QStringLiteral("p"),    QStringLiteral("g")};

/// The keys a material node keeps of a library entry: the basic ones and
/// its semiconductor models' parameters.
const QStringList& propertyKeys()
{
    static const QStringList keys = BasicKeys + semiconductorModelKeys();
    return keys;
}

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
    // Eg VBO Delta me γ1 γ2 γ3 Ep a0 ac av b C11 C12 εr n (eV, m0, Å; the
    // stiffness Vurgaftman et al.'s figures, 10⁹ dyn/cm² - their "GPa" 0.1 GPa).
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
        p.C11 = c11 * 1e8;
        p.C12 = c12 * 1e8;
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
        // (Indirect: its X gap 2.49 eV at 300 K; n √ε∞. Its heat Lumerical's.)
        {"AlP", bp(3.63, -1.74, 0.07, 0.22, 3.35, 0.71, 1.23, 17.7, 5.4672, -5.7, 3.0, -1.5, 1330, 630, 9.8, 2.75), 0.5771e-3, 372,
         110, 2850, 362, 80, 450},
    };
    return list;
}

/// A semiconductor's models' parameters, as Lumerical's database has them
/// (DEVICE's default materials, their lowest conduction valley's) - its
/// 1e-34 cm⁶/s Auger and 1e-20 cm³/s radiative coefficients standing for
/// none, as there. By carrier (electrons, holes): Caughey and Thomas's
/// µmin (cm²/(V s)), Nref (cm⁻³), α (0: none) and their exponents (T/300
/// K)^η; the lattice mobility's ζ, (T/300 K)^-ζ; the saturation velocity
/// (cm/s) and β; Slotboom's narrowing - both bands' V1 (eV), N0 (cm⁻³), C
/// (V1 0: none); the SRH lifetimes' exponent; Fossum's N0 (cm⁻³) by
/// carrier, A, B, C, E; the tunnelling masses (m0); Auger's coefficients
/// (cm⁶/s); the radiative one (cm³/s) and its exponent; Selberherr's α∞
/// (1/cm), Ecrit (V/cm), β by carrier (α∞ 0: none); Hurkx's band-to-band
/// A, B (V/cm), γ (A 0: none). Silicon's are the features' own defaults
/// (Caughey and Thomas's, Klaassen's narrowing, van Overstraeten and de
/// Man's ionization), their temperature exponents Lumerical's.
struct ModelSet {
    const char* name;
    double muMin[2], nRef[2], alpha[2], etaMuMin[2], etaNRef[2], etaAlpha[2];
    double zeta[2], vsat[2], beta[2];
    double bgn[3];
    double tauEta[2];
    double fossum[6];   // N0 n, N0 p, A, B, C, E
    double mt[2], auger[2], rad, radEta;
    double ii[6];       // α∞, Ecrit, β: electrons', holes'
    double bbt[3];
};

const std::vector<ModelSet>& modelSets()
{
    static const std::vector<ModelSet> list = {
        {"Si", {68.5, 44.9}, {9.2e16, 2.23e17}, {0.711, 0.719}, {-0.57, -0.57}, {2.4, 2.4}, {-0.146, -0.146}, {2.5, 2.2}, {1e7, 8e6}, {2, 1},
         {0.00692, 1.3e17, 0.5}, {0, 0}, {7.1e15, 7.1e15, 1, 0, 1, 1}, {0.25, 0.25}, {2.8e-31, 9.9e-32}, 1.1e-14, 0,
         {7.03e5, 1.231e6, 1, 1.582e6, 2.036e6, 1}, {4e14, 1.9e7, 2.5}},
        {"Ge", {850, 300}, {2.6e17, 1e17}, {0.56, 1}, {0, 0}, {0, 0}, {0, 0}, {1.66, 2.33}, {6e6, 5.4e6}, {2, 1}, {0, 0, 0}, {0, 0},
         {1e16, 1e16, 1, 0, 0, 1}, {0.5532, 0.19}, {1e-30, 1e-30}, 6.41e-14, 0, {1.55e7, 1.55698e6, 1, 2.15e5, 7.1e5, 1}, {9.1e16, 4.9e6, 2.5}},
        {"GaAs", {40, 40}, {8.1e16, 1e17}, {1, 1}, {-0.9, -0.9}, {0.5, 0.5}, {0, 0}, {2.2, 0.9}, {7.2e6, 9e6}, {2, 1}, {0.0045, 1.7e16, 0.5},
         {-0.5, -0.5}, {7.1e15, 7.1e15, 1, 0, 0, 1}, {0.0706, 0.574}, {1e-31, 2.28e-31}, 7.2e-10, -2.5, {2.99e5, 6.85e5, 1.6, 2.22e5, 6.57e5, 1.75},
         {0, 0, 0}},
        {"AlAs", {10, 5}, {5e17, 2.9e17}, {1, 1}, {0, 0}, {0, 0.5}, {0, 0}, {2.1, 2.2}, {8.5e6, 8e6}, {2, 2}, {0, 0, 0}, {0, 0},
         {1e20, 1e20, 1, 0, 0, 1}, {0.38, 0.81}, {1e-34, 1e-34}, 1e-20, 0, {3.69e5, 1.43e6, 1.33, 1.99e5, 1.21e6, 1.56}, {0, 0, 0}},
        {"InAs", {11700, 48}, {4.4e16, 2.55e17}, {0.5, 1}, {-0.33, 0}, {3.6, 0.5}, {0, 0}, {1.7, 2.3}, {9e6, 5e6}, {2, 2}, {0, 0, 0}, {0, 0},
         {1e20, 1e20, 1, 0, 0, 1}, {0.023, 0.41}, {1e-26, 2.2e-27}, 2e-11, 0, {1.25e5, 3.6e4, 1, 0, 0, 0}, {0, 0, 0}},
        {"AlP", {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {1.7e7, 1e7}, {2, 2}, {0, 0, 0}, {0, 0}, {1e20, 1e20, 1, 0, 0, 1},
         {0.4, 0.703}, {1e-34, 1e-34}, 1e-20, 0, {0, 0, 0, 0, 0, 0}, {0, 0, 0}},
        {"InP", {1120, 24}, {4e16, 2.5e17}, {0.6, 1}, {2, 1.2}, {3.7, 0.47}, {0, 0}, {1.9, 1.2}, {6.7e6, 6e6}, {1.25, 1}, {0, 0, 0}, {0, 0},
         {1e20, 1e20, 1, 0, 0, 1}, {0.077, 0.64}, {1.7e-33, 9e-31}, 2e-10, 0, {2.93e6, 2.64e6, 1, 1.62e6, 2.11e6, 1}, {0, 0, 0}},
        {"GaP", {10, 27}, {4.4e18, 5e17}, {0.8, 1}, {-1.07, 0}, {1.8, 0}, {0, 0}, {1.94, 2}, {8.8e6, 5e6}, {1.25, 1}, {0, 0, 0}, {0, 0},
         {1e20, 1e20, 1, 0, 0, 1}, {0.5, 0.6}, {1e-30, 1e-30}, 3e-15, 0, {0, 0, 0, 0, 0, 0}, {0, 0, 0}},
        {"AlN", {297, 0}, {1e17, 0}, {1.16, 0}, {-1.82, 0}, {3.78, 0}, {0.86, 0}, {1.26, 0}, {1.6e7, 1e7}, {2, 2}, {0, 0, 0}, {0, 0},
         {1e20, 1e20, 1, 0, 0, 1}, {0.314, 1.59}, {1e-34, 1e-34}, 4e-11, 0, {8.87e6, 3.76e7, 1, 0, 0, 0}, {0, 0, 0}},
        {"InN", {834, 0}, {2.88e17, 0}, {0.76, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {1.32e7, 1e7}, {2, 2}, {0, 0, 0}, {0, 0},
         {1e20, 1e20, 1, 0, 0, 1}, {0.11, 1.65}, {1e-34, 1e-34}, 2.2e-10, 0, {6.2e5, 3.54e7, 1, 0, 0, 0}, {0, 0, 0}},
        {"GaN", {295, 3}, {2e17, 3e17}, {0.66, 2}, {-1.02, 0}, {3.02, 0}, {0.81, 0}, {1.26, 2}, {1.34e7, 1e7}, {2, 2}, {0, 0, 0}, {0, 0},
         {1e20, 1e20, 1, 0, 0, 1}, {0.2, 1.47}, {1e-30, 1e-31}, 1.5e-11, 0, {1.4e7, 1.48e7, 1.4, 2.95e5, 5.17e6, 2}, {0, 0, 0}},
        {"GaSb", {680, 85}, {3.2e17, 3.04e17}, {1.08, 0.44}, {0, 0}, {0, 0}, {0, 0}, {1.5, 1.5}, {9e6, 1e7}, {2, 1}, {0, 0, 0}, {0, 0},
         {1e20, 1e20, 1, 0, 0, 1}, {0.0512, 0.42}, {5e-30, 5e-30}, 1e-10, 0, {1.07e7, 4.03e5, 1, 7.16e5, 4.02e5, 1}, {0, 0, 0}},
        {"AlSb", {5, 50}, {6e18, 1e17}, {1.21, 1}, {0, 0}, {0, 0}, {0, 0}, {1.8, 1.95}, {1e7, 1e7}, {2, 2}, {0, 0, 0}, {0, 0},
         {1e20, 1e20, 1, 0, 0, 1}, {1.5, 0.77}, {1e-34, 1e-34}, 1e-20, 0, {0, 0, 0, 0, 0, 0}, {0, 0, 0}},
        {"InSb", {200, 48}, {2e17, 2.36e18}, {0.57, 0.39}, {0, 0}, {0, 0}, {0, 0}, {1.66, 1.8}, {5e7, 1e7}, {2, 2}, {0, 0, 0}, {0, 0},
         {1e20, 1e20, 1, 0, 0, 1}, {0.0197, 0.43}, {5e-26, 1e-34}, 5e-11, 0, {0, 0, 0, 0, 0, 0}, {0, 0, 0}},
        {"CdS", {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {1e7, 1e7}, {2, 2}, {0, 0, 0}, {0, 0}, {1e20, 1e20, 1, 0, 0, 1},
         {0.21, 1.015}, {1e-34, 1e-34}, 1e-20, 0, {0, 0, 0, 0, 0, 0}, {0, 0, 0}},
        {"CdSe", {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {1e7, 1e7}, {2, 2}, {0, 0, 0}, {0, 0}, {1e20, 1e20, 1, 0, 0, 1},
         {0.13, 1.1}, {1e-34, 1e-34}, 1e-20, 0, {0, 0, 0, 0, 0, 0}, {0, 0, 0}},
        {"CdTe", {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {1e7, 1e7}, {2, 2}, {0, 0, 0}, {0, 0}, {1e20, 1e20, 1, 0, 0, 1},
         {0.11, 0.76}, {1e-34, 1e-34}, 1e-20, 0, {0, 0, 0, 0, 0, 0}, {0, 0, 0}},
    };
    return list;
}

const ModelSet* modelSet(const QString& name)
{
    for (const ModelSet& m : modelSets())
        if (name.compare(QLatin1String(m.name), Qt::CaseInsensitive) == 0) return &m;
    return nullptr;
}

/// A conductivity of heat's exponent, κ(T) = κ(300 K) (T/300 K)^η
/// (Palankovski's, whose κ at 300 K Lumerical's database has).
double heatExponent(const QString& name)
{
    static const std::pair<const char*, double> table[] = {{"Si", -1.65}, {"Ge", -1.25}, {"GaAs", -1.25}, {"AlAs", -1.37},
                                                           {"InAs", -1.1}, {"InP", -1.4},  {"GaP", -1.4}};
    for (const auto& [n, eta] : table)
        if (name.compare(QLatin1String(n), Qt::CaseInsensitive) == 0) return eta;
    return 0;
}

QString number(double v)
{
    return formatNumber(v, 10);
}

/// "v[unit]", times (T/300 K)^η where η is not 0.
QString ofT(double v, const QString& unit, double eta)
{
    QString t = unit.isEmpty() ? number(v) : QStringLiteral("%1[%2]").arg(number(v), unit);
    if (eta != 0) t += QStringLiteral("*(T/300[K])^(%1)").arg(number(eta));
    return t;
}

/// Varshni's gap anchored at its 300 K value: Eg(300 K) - α (T²/(T + β) -
/// 300²/(300 + β)) - the two terms the same at 300 K, to the last digit.
QString varshniOfT(double eg300, double alpha, double beta)
{
    if (alpha == 0) return QStringLiteral("%1[eV]").arg(number(eg300));
    return QStringLiteral("%1[eV] - %2[eV/K]*(T^2/(T + %3[K]) - (300[K])^2/(300[K] + %3[K]))").arg(number(eg300), number(alpha), number(beta));
}

/// Model set \a m's parameters as a material's properties (the keys
/// semiconductorModelKeys() names; what it has none of left out).
void addModels(QJsonObject& p, const ModelSet& m)
{
    auto put = [&p](const char* key, const QString& v) { p.insert(QString::fromLatin1(key), v); };
    const char* c[2] = {"n", "p"};
    for (int k = 0; k < 2; ++k) {
        const QString C = QLatin1String(c[k]);
        if (m.nRef[k] > 0) {
            p.insert(QStringLiteral("mu") + C + QStringLiteral("min"), ofT(m.muMin[k], QStringLiteral("cm^2/(V*s)"), m.etaMuMin[k]));
            p.insert(QStringLiteral("nref") + C, ofT(m.nRef[k], QStringLiteral("1/cm^3"), m.etaNRef[k]));
            p.insert(QStringLiteral("alpha") + C, ofT(m.alpha[k], QString(), m.etaAlpha[k]));
        }
        if (m.zeta[k] != 0) p.insert(QStringLiteral("zeta") + C, number(m.zeta[k]));
        p.insert(QStringLiteral("vsat") + C, ofT(m.vsat[k], QStringLiteral("cm/s"), 0));
        p.insert(QStringLiteral("beta") + C, number(m.beta[k]));
        p.insert(QStringLiteral("N0") + C, ofT(m.fossum[k], QStringLiteral("1/cm^3"), 0));
        p.insert(QStringLiteral("mt") + C, number(m.mt[k]));
        p.insert(QStringLiteral("cau") + C, ofT(m.auger[k], QStringLiteral("cm^6/s"), 0));
        if (m.ii[3 * k] > 0) {
            p.insert(QStringLiteral("iia") + C, ofT(m.ii[3 * k], QStringLiteral("1/cm"), 0));
            p.insert(QStringLiteral("iib") + C, ofT(m.ii[3 * k + 1], QStringLiteral("V/cm"), 0));
            p.insert(QStringLiteral("iibeta") + C, number(m.ii[3 * k + 2]));
        }
    }
    if (m.bgn[0] > 0) {
        put("Ebgn", ofT(m.bgn[0], QStringLiteral("eV"), 0));
        put("Nbgn", ofT(m.bgn[1], QStringLiteral("1/cm^3"), 0));
        put("Cbgn", number(m.bgn[2]));
    }
    put("fA", number(m.fossum[2]));
    put("fB", number(m.fossum[3]));
    put("fC", number(m.fossum[4]));
    put("fE", number(m.fossum[5]));
    put("copt", ofT(m.rad, QStringLiteral("cm^3/s"), m.radEta));
    if (m.bbt[0] > 0) {
        put("bbtA", number(m.bbt[0]));
        put("bbtB", ofT(m.bbt[1], QStringLiteral("V/cm"), 0));
        put("bbtgamma", number(m.bbt[2]));
    }
}

// Indices of the wavelength (lambda0), each checked against the tables
// Lumerical's database has (Palik's, Johnson and Christy's, the CRC's).

/// Sellmeier's n² = A + Σ B λ²/(λ² - C²), C in µm.
QString sellmeier(double A, std::initializer_list<std::pair<double, double>> terms)
{
    QString t = QStringLiteral("sqrt(%1").arg(number(A));
    for (const auto& [B, C] : terms)
        t += QStringLiteral(" + %1*lambda0^2/(lambda0^2 - %2[um^2])").arg(formatNumber(B, 9), formatNumber(C * C, 9));
    return t + QLatin1Char(')');
}

/// Sellmeier's held at its range's ends (µm): its poles beyond them.
QString heldSellmeier(double A, std::initializer_list<std::pair<double, double>> terms, double from, double to)
{
    return sellmeier(A, terms).replace(QStringLiteral("lambda0"), QStringLiteral("min(max(lambda0, %1[um]), %2[um])").arg(number(from), number(to)));
}

/// Kato and Takaoka's form: n² = A + B/(λ² - C) + D/(λ² - E), λ in µm.
QString katoTakaoka(double A, double B, double C, double D, double E)
{
    return QStringLiteral("sqrt(%1 + %2[um^2]/(lambda0^2 - %3[um^2]) + %4[um^2]/(lambda0^2 - %5[um^2]))")
        .arg(number(A), number(B), number(C), number(D), number(E));
}

/// n of the wavelength through measured points (µm, n), straight between.
QString tabulated(std::initializer_list<std::pair<double, double>> points)
{
    QString t = QStringLiteral("interp(lambda0");
    for (const auto& [l, n] : points) t += QStringLiteral(", %1[um], %2").arg(number(l), number(n));
    return t + QLatin1Char(')');
}

// A crystal's tensors as a material's texts - Voigt's entries, each named
// (fem_tensor.h) - from its independent constants, its class putting them
// where they go: those the same, of the opposite sign or a combination of
// others named too. The crystal's axes: x1 the a axis (a mirror's normal
// in 3m), x3 the c axis (the optic axis of a uniaxial one).

struct Voigt {
    int row, col;
    double value;
};

/// "C11=v[unit]; ..." of the entries not nought.
QString voigtEntries(char letter, std::initializer_list<Voigt> entries, const char* unit)
{
    QStringList items;
    for (const Voigt& v : entries)
        if (v.value != 0)
            items << QStringLiteral("%1%2%3=%4%5")
                         .arg(QLatin1Char(letter))
                         .arg(v.row)
                         .arg(v.col)
                         .arg(number(v.value), *unit ? QStringLiteral("[%1]").arg(QLatin1String(unit)) : QString());
    return items.join(QLatin1String("; "));
}

/// A cubic crystal's stiffness (GPa).
QString cubicStiffness(double c11, double c12, double c44)
{
    return voigtEntries('C', {{1, 1, c11}, {2, 2, c11}, {3, 3, c11}, {1, 2, c12}, {1, 3, c12}, {2, 3, c12}, {4, 4, c44}, {5, 5, c44}, {6, 6, c44}},
                        "GPa");
}

/// A trigonal crystal's (3m, 32, -3m; GPa): C66 = (C11 - C12)/2.
QString trigonalStiffness(double c11, double c12, double c13, double c14, double c33, double c44)
{
    return voigtEntries('C',
                        {{1, 1, c11}, {1, 2, c12}, {1, 3, c13}, {1, 4, c14}, {2, 2, c11}, {2, 3, c13}, {2, 4, -c14}, {3, 3, c33}, {4, 4, c44},
                         {5, 5, c44}, {5, 6, c14}, {6, 6, (c11 - c12) / 2}},
                        "GPa");
}

/// 3m's piezoelectric strain constants (pC/N): d16 = -2 d22 (an
/// engineering shear's).
QString trigonal3mStrainPiezo(double d15, double d22, double d31, double d33)
{
    return voigtEntries('d', {{1, 5, d15}, {1, 6, -2 * d22}, {2, 1, -d22}, {2, 2, d22}, {2, 4, d15}, {3, 1, d31}, {3, 2, d31}, {3, 3, d33}}, "pC/N");
}

/// 32's (quartz's; pC/N).
QString trigonal32StrainPiezo(double d11, double d14)
{
    return voigtEntries('d', {{1, 1, d11}, {1, 2, -d11}, {1, 4, d14}, {2, 5, -d14}, {2, 6, -2 * d11}}, "pC/N");
}

/// 3m's electro-optic coefficients (pm/V).
QString trigonal3mElectroOptic(double r13, double r22, double r33, double r51)
{
    return voigtEntries('r', {{1, 2, -r22}, {1, 3, r13}, {2, 2, r22}, {2, 3, r13}, {3, 3, r33}, {4, 2, r51}, {5, 1, r51}, {6, 1, -r22}}, "pm/V");
}

/// mm2's (KTP's; pm/V).
QString mm2ElectroOptic(double r13, double r23, double r33, double r42, double r51)
{
    return voigtEntries('r', {{1, 3, r13}, {2, 3, r23}, {3, 3, r33}, {4, 2, r42}, {5, 1, r51}}, "pm/V");
}

/// -43m's (zinc blende's; pm/V): r41 = r52 = r63.
QString zincBlendeElectroOptic(double r41)
{
    return voigtEntries('r', {{4, 1, r41}, {5, 2, r41}, {6, 3, r41}}, "pm/V");
}

/// A cubic crystal's photoelastic coefficients - an isotropic solid's with
/// p44 = (p11 - p12)/2.
QString cubicPhotoelastic(double p11, double p12, double p44)
{
    return voigtEntries('p', {{1, 1, p11}, {2, 2, p11}, {3, 3, p11}, {1, 2, p12}, {1, 3, p12}, {2, 3, p12}, {4, 4, p44}, {5, 5, p44}, {6, 6, p44}},
                        "");
}

/// A trigonal crystal's (3m, 32, -3m): p66 = (p11 - p12)/2; p not symmetric
/// (p13 and p31, p14 and p41 apart).
QString trigonalPhotoelastic(double p11, double p12, double p13, double p14, double p31, double p33, double p41, double p44)
{
    return voigtEntries('p',
                        {{1, 1, p11}, {1, 2, p12}, {1, 3, p13}, {1, 4, p14}, {2, 1, p12}, {2, 2, p11}, {2, 3, p13}, {2, 4, -p14}, {3, 1, p31},
                         {3, 2, p31}, {3, 3, p33}, {4, 1, p41}, {4, 2, -p41}, {4, 4, p44}, {5, 5, p44}, {5, 6, p41}, {6, 5, p14},
                         {6, 6, (p11 - p12) / 2}},
                        "");
}

/// κ of the wavelength through measured points (µm, κ) over decades:
/// straight between their logarithms, nought beyond the last.
QString tabulatedLog(std::initializer_list<std::pair<double, double>> points)
{
    QString t = QStringLiteral("interp(lambda0");
    double last = 0;
    for (const auto& [l, k] : points) {
        t += QStringLiteral(", %1[um], %2").arg(number(l), formatNumber(std::log(k), 6));
        last = l;
    }
    return QStringLiteral("if(lambda0 > %1[um], 0, exp(%2)))").arg(number(last), t);
}

/// Silicon's index: Green's measured n below 0.4 µm (its E1 peak there),
/// above it a Sellmeier of two ultraviolet poles fitted to Green's (0.4-1.45
/// µm, 300 K) and Li's (1.2-14 µm, 293 K) together - within 0.007 of either,
/// 1e-4 of Li's at 1550 nm, falling with λ, no pole in its range.
QString siliconIndex()
{
    const QString uv = tabulated({{0.25, 1.665}, {0.26, 1.757}, {0.27, 2.068}, {0.28, 2.959}, {0.29, 4.356}, {0.30, 4.976}, {0.31, 5.121},
                                  {0.32, 5.112}, {0.33, 5.195}, {0.34, 5.301}, {0.35, 5.494}, {0.36, 6.026}, {0.37, 6.891}, {0.38, 6.616},
                                  {0.39, 6.039}, {0.40, 5.613}});
    return QStringLiteral("if(lambda0 < 0.4[um], %1, %2)").arg(uv, sellmeier(1, {{5.763557701, 0.2623529368}, {4.885714572, 0.3488033962}}));
}

/// Silicon's absorption at 300 K, Green's (its band-to-band absorption,
/// self-consistent with its n) from 0.25 to 1.45 µm; nought beyond.
QString siliconExtinction()
{
    return tabulatedLog({
        {0.25, 3.665}, {0.26, 4.084}, {0.27, 4.68}, {0.28, 5.287}, {0.29, 5.286}, {0.30, 4.234}, {0.31, 3.598}, {0.32, 3.303},
        {0.33, 3.1}, {0.34, 2.977}, {0.35, 2.938}, {0.36, 2.966}, {0.37, 2.171}, {0.38, 0.946}, {0.39, 0.445}, {0.40, 0.296},
        {0.41, 0.227}, {0.42, 0.176}, {0.43, 0.138}, {0.44, 0.107}, {0.45, 0.086302}, {0.46, 0.071381}, {0.47, 0.062086}, {0.48, 0.055004},
        {0.49, 0.049131}, {0.50, 0.044165}, {0.51, 0.039367}, {0.52, 0.036415}, {0.53, 0.033108}, {0.54, 0.030295}, {0.55, 0.027968}, {0.56, 0.025758},
        {0.57, 0.024131}, {0.58, 0.022524}, {0.59, 0.021081}, {0.60, 0.019934}, {0.61, 0.018446}, {0.62, 0.017367}, {0.63, 0.016444}, {0.64, 0.015432},
        {0.65, 0.014431}, {0.66, 0.013498}, {0.67, 0.012743}, {0.68, 0.011905}, {0.69, 0.011201}, {0.70, 0.010528}, {0.71, 0.010057}, {0.72, 0.0096257},
        {0.73, 0.0089461}, {0.74, 0.008362}, {0.75, 0.0078185}, {0.76, 0.007197}, {0.77, 0.0067402}, {0.78, 0.0063933}, {0.79, 0.005834}, {0.80, 0.0054113},
        {0.81, 0.0049955}, {0.82, 0.0046134}, {0.83, 0.0042734}, {0.84, 0.0039439}, {0.85, 0.003612}, {0.86, 0.0032781}, {0.87, 0.0029839}, {0.88, 0.0026821},
        {0.89, 0.0024293}, {0.90, 0.0021701}, {0.91, 0.0019625}, {0.92, 0.0017571}, {0.93, 0.0015467}, {0.94, 0.0013689}, {0.95, 0.0011793}, {0.96, 0.0010237},
        {0.97, 0.00087225}, {0.98, 0.00074866}, {0.99, 0.00062238}, {1.00, 0.0005093}, {1.01, 0.00041071}, {1.02, 0.00032386}, {1.03, 0.00024753}, {1.04, 0.00018704},
        {1.05, 0.0001362}, {1.06, 9.3631e-05}, {1.07, 6.8118e-05}, {1.08, 5.3285e-05}, {1.09, 4.0768e-05}, {1.10, 3.0637e-05}, {1.11, 2.3849e-05}, {1.12, 1.7825e-05},
        {1.13, 1.3488e-05}, {1.14, 9.0718e-06}, {1.15, 6.223e-06}, {1.16, 3.877e-06}, {1.17, 2.0483e-06}, {1.18, 6.1036e-07}, {1.19, 3.4091e-07}, {1.20, 2.1008e-07},
        {1.21, 1.2518e-07}, {1.22, 7.9609e-08}, {1.23, 4.6004e-08}, {1.24, 2.3682e-08}, {1.25, 9.9472e-09}, {1.26, 3.6096e-09}, {1.27, 2.0213e-09}, {1.28, 1.2223e-09},
        {1.29, 7.2885e-10}, {1.30, 4.6553e-10}, {1.31, 2.8147e-10}, {1.32, 1.6807e-10}, {1.33, 8.467e-11}, {1.34, 3.7322e-11}, {1.35, 1.8263e-11}, {1.36, 1.0281e-11},
        {1.37, 6.5413e-12}, {1.38, 4.173e-12}, {1.39, 2.5441e-12}, {1.40, 1.5597e-12}, {1.41, 9.5374e-13}, {1.42, 5.65e-13}, {1.43, 2.8449e-13}, {1.44, 2.0626e-13},
        {1.45, 1.3846e-13}
    });
}

/// Germanium's index: Nunley et al.'s measured n (J. Vac. Sci. Technol. B
/// 34, 061205, 2016; 300 K) to 1.9 µm - its points where a straight line
/// between them misses those left out by 0.002 at most -, Burnett et al.'s
/// Sellmeier (2-14 µm) from 2 µm, the two weighed between (0.005 apart).
QString germaniumIndex()
{
    const QString near = tabulated({
        {0.3002, 3.916}, {0.3046, 3.938}, {0.3179, 3.992}, {0.3271, 4.015}, {0.3454, 4.043}, {0.3522, 4.063}, {0.3583, 4.092},
        {0.3701, 4.163}, {0.3757, 4.184}, {0.3887, 4.21}, {0.3936, 4.209}, {0.3987, 4.197}, {0.4065, 4.161}, {0.4175, 4.107},
        {0.4246, 4.082}, {0.432, 4.07}, {0.4381, 4.069}, {0.4476, 4.084}, {0.4575, 4.117}, {0.4679, 4.167}, {0.4787, 4.235},
        {0.4881, 4.306}, {0.4959, 4.376}, {0.502, 4.439}, {0.5061, 4.488}, {0.5123, 4.578}, {0.5166, 4.65}, {0.5209, 4.731},
        {0.5298, 4.91}, {0.5344, 4.996}, {0.5391, 5.072}, {0.5414, 5.105}, {0.5462, 5.16}, {0.5486, 5.183}, {0.556, 5.236}, {0.561,
        5.27}, {0.5636, 5.292}, {0.5661, 5.319}, {0.5687, 5.353}, {0.5714, 5.396}, {0.574, 5.446}, {0.5821, 5.625}, {0.5848, 5.68},
        {0.5876, 5.725}, {0.5904, 5.758}, {0.5932, 5.778}, {0.5961, 5.785}, {0.599, 5.78}, {0.6019, 5.766}, {0.6048, 5.745},
        {0.6108, 5.69}, {0.6326, 5.468}, {0.6458, 5.348}, {0.6525, 5.292}, {0.663, 5.214}, {0.6775, 5.122}, {0.6965, 5.022},
        {0.7167, 4.935}, {0.738, 4.856}, {0.7606, 4.785}, {0.7798, 4.733}, {0.8051, 4.674}, {0.8321, 4.621}, {0.861, 4.573}, {0.892,
        4.53}, {0.9322, 4.484}, {0.984, 4.438}, {1.042, 4.397}, {1.107, 4.364}, {1.216, 4.31}, {1.319, 4.275}, {1.425, 4.249},
        {1.494, 4.241}, {1.531, 4.249}, {1.55, 4.25}, {1.59, 4.216}, {1.631, 4.194}, {1.722, 4.164}, {1.851, 4.136}, {2.033, 4.109}
    });
    const QString ir = QStringLiteral("sqrt(1 + 0.4886331*lambda0^2/(lambda0^2 - 1.393959[um^2]) + 14.5142535*lambda0^2/(lambda0^2 - "
                                      "0.1626427[um^2]) + 0.0091224*lambda0^2/(lambda0^2 - 752.19[um^2]))");
    return QStringLiteral("if(lambda0 < 1.9[um], %1, if(lambda0 > 2[um], %2, ((2[um] - lambda0)*%1 + (lambda0 - 1.9[um])*%2)/0.1[um]))").arg(near, ir);
}

/// Germanium's absorption at 300 K, Nunley et al.'s (its direct gap's edge at
/// 1550 nm) to 2.03 µm, within 3 % of their points; nought beyond.
QString germaniumExtinction()
{
    return tabulatedLog({
        {0.3002, 3.694}, {0.3342, 2.845}, {0.4175, 2.097}, {0.5276, 2.385}, {0.5821, 1.826}, {0.623, 0.8524}, {0.6595, 0.5599},
        {0.7293, 0.3597}, {0.8321, 0.247}, {1.127, 0.1292}, {1.512, 0.05406}, {1.531, 0.04564}, {1.55, 0.02519}, {1.569, 0.007651},
        {1.59, 0.002758}, {1.61, 0.001915}, {1.722, 0.0004261}, {1.851, 5.835e-05}, {2.033, 2.607e-06}
    });
}

/// Afromowitz's index of an oscillator E0, Ed and a gap Eg (eV; Solid State
/// Commun. 15, 59, 1974) with lattice terms (εs - ε∞, TO phonon); at 0.995 Eg
/// and above, held (within the measured spectra's spread to 0.1 eV over the
/// gap: GaAs's n there differs by 0.06 from one to another).
QString oscillatorIndex(double E0, double Ed, double Eg, const std::vector<std::pair<double, double>>& lattice = {})
{
    const double eta = M_PI * Ed / (2 * E0 * E0 * E0 * (E0 * E0 - Eg * Eg));
    const QString E = QStringLiteral("min(1.23984198[um]/lambda0, %1)").arg(number(0.995 * Eg));
    QString t = QStringLiteral("%1 + %2*%3^2 + %4*%3^4*log((%5 - %3^2)/(%6 - %3^2))")
                    .arg(number(1 + Ed / E0), number(Ed / (E0 * E0 * E0)), E, number(eta / M_PI), number(2 * E0 * E0 - Eg * Eg), number(Eg * Eg));
    for (const auto& [S, ETO] : lattice)
        if (S > 0) t += QStringLiteral(" + %1/(1 - (1.23984198[um]/lambda0/%2)^2)").arg(number(S), number(ETO));
    return QStringLiteral("sqrt(%1)").arg(t);
}

/// Adachi's index of InGaAsP on InP's lattice (J. Appl. Phys. 53, 5863,
/// 1982): n² = A [f(E/E0) + ½ (E0/Eso)^1.5 f(E/Eso)] + B, f(χ) = χ⁻² (2 -
/// (1 + χ)^½ - (1 - χ)^½), A 8.4 - 3.4 y, B 6.6 + 3.4 y - his 1989
/// dispersion's (the database's y 0.24) within 0.004 from 1.55 to 5 µm, 0.01
/// at 1.3 µm, nearer the gap; InP's
/// (y 0) Pettit and Turner's within 0.002 at 1550 nm; held at 0.995 E0 and
/// above.
QString adachiIndex(double y, double E0, double Eso)
{
    const double A = 8.4 - 3.4 * y, B = 6.6 + 3.4 * y;
    const QString E = QStringLiteral("min(1.23984198[um]/lambda0, %1)").arg(number(0.995 * E0));
    auto f = [&](double gap) {
        const QString c = QStringLiteral("(%1/%2)").arg(E, number(gap));
        return QStringLiteral("(2 - sqrt(1 + %1) - sqrt(1 - %1))/%1^2").arg(c);
    };
    return QStringLiteral("sqrt(%1*(%2 + %3*%4) + %5)").arg(number(A), f(E0), number(0.5 * std::pow(E0 / Eso, 1.5)), f(Eso), number(B));
}

/// Afromowitz's AlxGa1-xAs: GaAs Palik's within 0.004 from 0.9 to 2 µm.
QString afromowitzIndex(double x)
{
    return oscillatorIndex(3.65 + 0.871 * x + 0.179 * x * x, 36.1 - 2.45 * x, 1.424 + 1.266 * x + 0.26 * x * x);
}

/// Water's index at 20 °C: Daimon and Masumura's Sellmeier (Appl. Opt. 46,
/// 3811, 2007) to 1 µm, Hale and Querry's (25 °C; Appl. Opt. 12, 555, 1973)
/// from 1.13 µm - its points within 0.001 -, the two weighed between (0.0015
/// apart: their temperatures).
QString waterIndex()
{
    const QString daimon = sellmeier(1, {{0.5684027565, std::sqrt(5.101829712e-3)}, {0.1726177391, std::sqrt(1.821153936e-2)},
                                         {0.02086189578, std::sqrt(2.620722293e-2)}, {0.1130748688, std::sqrt(10.69792721)}});
    const QString hale = tabulated({
        {1, 1.327}, {1.6, 1.317}, {2, 1.306}, {2.2, 1.296}, {2.4, 1.279}, {2.6, 1.242}, {2.65, 1.219}, {2.75, 1.157}, {2.8, 1.142},
        {2.85, 1.149}, {2.9, 1.201}, {2.95, 1.292}, {3, 1.371}, {3.05, 1.426}, {3.1, 1.467}, {3.15, 1.483}, {3.2, 1.478}, {3.25,
        1.467}, {3.35, 1.432}, {3.4, 1.42}, {3.5, 1.4}, {3.6, 1.385}, {3.8, 1.364}, {4, 1.351}, {4.4, 1.334}, {4.6, 1.33}, {4.8,
        1.33}, {5.1, 1.322}, {5.3, 1.312}, {5.5, 1.298}, {5.6, 1.289}, {5.7, 1.277}, {5.9, 1.248}, {6, 1.265}, {6.1, 1.319}, {6.2,
        1.363}, {6.3, 1.357}, {6.5, 1.339}, {6.8, 1.324}, {7.1, 1.314}, {8.4, 1.281}, {9, 1.262}, {9.6, 1.239}, {10, 1.218}, {11,
        1.153}, {11.5, 1.126}, {12, 1.111}, {12.5, 1.123}, {13, 1.146}, {14.5, 1.241}, {16, 1.325}, {17.5, 1.401}, {18.5, 1.443},
        {19, 1.461}, {19.5, 1.476}, {21, 1.487}, {23, 1.511}, {25, 1.531}, {26, 1.539}, {28, 1.549}, {29, 1.551}, {30, 1.551}, {32,
        1.546}, {36, 1.527}, {40, 1.519}, {42, 1.522}, {44, 1.53}, {46, 1.541}, {48, 1.555}, {50, 1.587}, {70, 1.821}, {80, 1.886},
        {90, 1.924}, {100, 1.957}, {110, 1.966}, {120, 2.004}, {130, 2.036}, {140, 2.056}, {190, 2.119}, {200, 2.13}
    });
    return QStringLiteral("if(lambda0 < 1[um], %1, if(lambda0 > 1.129[um], %2, ((1.129[um] - lambda0)*%1 + (lambda0 - 1[um])*%2)/0.129[um]))")
        .arg(daimon, hale);
}

/// Water's absorption, Hale and Querry's from 0.2 to 200 µm (within 2 % of
/// their points).
QString waterExtinction()
{
    return tabulatedLog({
        {0.2, 1.1e-07}, {0.225, 4.9e-08}, {0.3, 1.6e-08}, {0.325, 1.08e-08}, {0.35, 6.5e-09}, {0.4, 1.86e-09}, {0.425, 1.3e-09},
        {0.45, 1.02e-09}, {0.475, 9.35e-10}, {0.5, 1e-09}, {0.525, 1.32e-09}, {0.55, 1.96e-09}, {0.575, 3.6e-09}, {0.6, 1.09e-08},
        {0.625, 1.39e-08}, {0.65, 1.64e-08}, {0.675, 2.23e-08}, {0.7, 3.35e-08}, {0.725, 9.15e-08}, {0.75, 1.56e-07}, {0.775,
        1.48e-07}, {0.8, 1.25e-07}, {0.825, 1.82e-07}, {0.85, 2.93e-07}, {0.875, 3.91e-07}, {0.9, 4.86e-07}, {0.925, 1.06e-06},
        {0.95, 2.93e-06}, {0.975, 3.48e-06}, {1, 2.89e-06}, {1.2, 9.89e-06}, {1.4, 0.000138}, {1.6, 8.55e-05}, {1.8, 0.000115}, {2,
        0.0011}, {2.2, 0.000289}, {2.6, 0.00317}, {2.65, 0.0067}, {2.7, 0.019}, {2.75, 0.059}, {2.8, 0.115}, {2.85, 0.185}, {2.9,
        0.268}, {2.95, 0.298}, {3.05, 0.24}, {3.1, 0.192}, {3.2, 0.0924}, {3.25, 0.061}, {3.3, 0.0368}, {3.35, 0.0261}, {3.4,
        0.0195}, {3.45, 0.0132}, {3.5, 0.0094}, {3.6, 0.00515}, {3.7, 0.0036}, {3.8, 0.0034}, {3.9, 0.0038}, {4.4, 0.0103}, {4.5,
        0.0134}, {4.7, 0.0157}, {4.8, 0.015}, {5.2, 0.0101}, {5.3, 0.0098}, {5.4, 0.0103}, {5.5, 0.0116}, {5.6, 0.0142}, {5.7,
        0.0203}, {5.8, 0.033}, {5.9, 0.0622}, {6, 0.107}, {6.1, 0.131}, {6.3, 0.057}, {6.4, 0.0449}, {6.6, 0.0356}, {6.8, 0.0327},
        {7.4, 0.0324}, {8.8, 0.0385}, {9.8, 0.0479}, {10, 0.0508}, {10.5, 0.0662}, {11.5, 0.142}, {12, 0.199}, {12.5, 0.259}, {13,
        0.305}, {13.5, 0.343}, {14.5, 0.388}, {16.5, 0.428}, {19, 0.414}, {22, 0.373}, {32, 0.324}, {36, 0.343}, {50, 0.514}, {60,
        0.587}, {90, 0.536}, {130, 0.514}, {160, 0.496}, {200, 0.504}
    });
}

/// Fused silica's absorption in the infrared: Franta et al.'s (a bulk plate's;
/// Proc. SPIE 9890, 989014, 2016) from 2.5 to 7 µm, within 3 % of their
/// points; nought below (its transparent window: 5e-6 at 2.5 µm).
QString silicaExtinction()
{
    return QStringLiteral("if(lambda0 < 2.5[um], 0, %1)").arg(tabulatedLog({
        {2.503, 5.05e-06}, {2.555, 1.06e-05}, {2.597, 1.54e-05}, {2.639, 2.15e-05}, {2.657, 2.93e-05}, {2.669, 4.13e-05}, {2.675,
        5.37e-05}, {2.682, 8.02e-05}, {2.694, 0.000258}, {2.7, 0.000411}, {2.706, 0.000536}, {2.713, 0.000595}, {2.731, 0.000597},
        {2.763, 0.000417}, {2.795, 0.000237}, {2.86, 5.04e-05}, {2.893, 2.73e-05}, {2.934, 1.58e-05}, {2.988, 9.65e-06}, {3.065,
        6.24e-06}, {3.254, 4.03e-06}, {3.322, 3.87e-06}, {3.376, 4.68e-06}, {3.463, 7.49e-06}, {3.551, 1.61e-05}, {3.651, 2.76e-05},
        {3.719, 4.39e-05}, {3.771, 4.97e-05}, {3.894, 5.12e-05}, {3.994, 5.92e-05}, {4.153, 0.000112}, {4.25, 0.000147}, {4.359,
        0.00027}, {4.41, 0.000304}, {4.471, 0.000285}, {4.554, 0.000241}, {4.617, 0.000265}, {4.703, 0.000379}, {4.758, 0.000585},
        {4.88, 0.00247}, {4.936, 0.00383}, {4.993, 0.00471}, {5.075, 0.0048}, {5.157, 0.00476}, {5.217, 0.00595}, {5.302, 0.00901},
        {5.351, 0.00958}, {5.413, 0.00825}, {5.526, 0.00534}, {5.629, 0.00473}, {5.786, 0.00488}, {5.935, 0.00625}, {6.087, 0.0084},
        {6.186, 0.0084}, {6.389, 0.0065}, {6.537, 0.00721}, {6.989, 0.0105}
    }));
}

/// A zinc blende binary's optics (fitted to the refractiveindex.info
/// database's measured spectra at 300 K, the gaps the measured ones):
/// Afromowitz's oscillator E0, Ed (eV) at its gap, the lattice's term
/// εs - ε∞ at its TO phonon (eV), below the gap; its direct gap's
/// absorption α = A √(E - Eg) / E (1/cm, E in eV) above it - an alloy's
/// strength A its binaries' over µ^1.5 Ep weighed, of its own µ and Ep (the
/// edge's joint density of states); the longest wavelength fitted (µm).
struct BinaryOptics {
    const char* name;
    double E0, Ed, S, ETO, A, to;
};

const std::vector<BinaryOptics>& binaryOptics()
{
    static const std::vector<BinaryOptics> list = {
        // (n: Skauli's GaAs, Pettit and Turner's InP, Lorimor's InAs, Fern and
        // Onton's AlAs, Bond's GaP, Zollner's and Adachi's AlSb, Djurišić's
        // and Adachi's GaSb - 0.15 apart at 2 µm, the fit between -, Adachi's
        // InSb, below their gaps; A: Aspnes and Studna's GaAs and InP,
        // Jellison's and Aspnes's GaP, Djurišić's GaSb, Zollner's and
        // Djurišić's AlSb, Rakić's AlAs, Adachi's InAs and InSb, from 0.05 to
        // 0.45 eV above the gap - InP's 1.5-1.7 eV, its Afk.)
        {"GaAs", 3.6795, 36.290, 2.01, 0.0333, 6.578e4, 17},
        {"InP", 3.4589, 29.597, 2.89, 0.0377, 1.16e5, 10},
        {"InAs", 1.5317, 16.533, 2.85, 0.0271, 9381, 20},
        {"GaSb", 2.1130, 26.897, 1.30, 0.0284, 2.806e4, 12.4},
        {"InSb", 1.0841, 15.682, 1.10, 0.0222, 3449, 12.4},
        {"AlSb", 3.4606, 31.384, 1.80, 0.0395, 7.22e5, 12.4},
        {"GaP", 4.9666, 40.393, 1.99, 0.0455, 5.051e5, 4},
        {"AlAs", 4.7906, 34.333, 1.90, 0.0449, 4.486e5, 2.2},
        // (None measured in the database: its oscillator at 1.65 its Γ gap,
        // AlAs's and GaP's ratio, Ed to its ε∞ 7.56; A of AlAs's and GaP's
        // µ^1.5 Ep's mean.)
        {"AlP", 5.862, 38.47, 2.24, 0.0548, 6.0e5, 2.2},
    };
    return list;
}

const BinaryOptics* opticsOf(const char* name)
{
    for (const BinaryOptics& o : binaryOptics())
        if (qstrcmp(o.name, name) == 0) return &o;
    return nullptr;
}

/// The edge's strength's scale: the reduced mass of the electron and the
/// heavy hole (along [001]) to the 1.5, times Ep.
double edgeScale(const BandParameters& p)
{
    const double mh = 1 / (p.gamma1 - 2 * p.gamma2), mr = p.me * mh / (p.me + mh);
    return std::pow(mr, 1.5) * p.Ep;
}

/// κ of a direct gap's absorption α = A √(E - Eg) / E above Eg (A in 1/cm,
/// E in eV), αλ/4π; nought below.
QString edgeExtinction(double A, double Eg)
{
    const QString E = QStringLiteral("(1.23984198[um]/lambda0)");
    return QStringLiteral("%1[1/cm]*lambda0/(4*pi)*sqrt(max(%2 - %3, 0))/%2").arg(number(A), E, number(Eg));
}

/// A metal's permittivity as Drude's and Lorentz's terms of the photon's
/// energy w (eV): ε∞ - ωp² f0/(w (w + iΓ0)) + Σ ωp² fj/(ωj² - w² - iΓj w);
/// its n or κ (\a extinction).
QString lorentzDrude(double epsInf, double wp, double f0, double g0, std::initializer_list<std::array<double, 3>> oscillators, bool extinction)
{
    const QString w = QStringLiteral("(1.23984198[um]/lambda0)");
    const double d = wp * wp * f0;
    QString e1 = QStringLiteral("%1 - %2/(%3^2 + %4)").arg(number(epsInf), number(d), w, number(g0 * g0));
    QString e2 = QStringLiteral("%1/(%2*(%2^2 + %3))").arg(number(d * g0), w, number(g0 * g0));
    for (const auto& [f, wj, gj] : oscillators) {
        const QString D = QStringLiteral("((%1 - %2^2)^2 + %3*%2^2)").arg(number(wj * wj), w, number(gj * gj));
        e1 += QStringLiteral(" + %1*(%2 - %3^2)/%4").arg(number(wp * wp * f), number(wj * wj), w, D);
        e2 += QStringLiteral(" + %1*%2/%3").arg(number(wp * wp * f * gj), w, D);
    }
    return QStringLiteral("%1(%2, %3)").arg(extinction ? QStringLiteral("keps") : QStringLiteral("neps"), e1, e2);
}

/// The metals' optical constants (n or κ): gold Vial et al.'s
/// Drude-Lorentz fit (Phys. Rev. B 71, 085416, 2005) - Johnson and Christy's
/// κ within 3 % from 0.55 to 1.9 µm -, silver a Drude-Lorentz fit of ours to
/// Yang et al.'s (Phys. Rev. B 91, 235137, 2015; ε∞ 2.45, ωp 8.85 eV, γ 37
/// meV, an oscillator at 8 eV for its interband absorption) - n and κ within
/// 3 % from 0.4 to 1 µm, 1 % from 1 to 25 µm; Johnson and Christy's κ within
/// 5 % -,
/// copper and aluminium Rakić et al.'s Lorentz-Drude (Appl. Opt. 37, 5271,
/// 1998) - the CRC's κ within 5 % from 0.6 to 2 µm (their n 10-40 % off).
QString metalOptics(const char* metal, bool extinction)
{
    const QLatin1String m(metal);
    if (m == QLatin1String("Au"))
        return lorentzDrude(5.9673, 8.74116, 1.0, 0.0658413, {{1.09 * 2.68848 * 2.68848 / (8.74116 * 8.74116), 2.68848, 0.433666}}, extinction);
    if (m == QLatin1String("Ag")) return lorentzDrude(2.4473, 8.847, 1.0, 0.03671, {{0.89276 * 64 / (8.847 * 8.847), 8, 3}}, extinction);
    if (m == QLatin1String("Cu"))
        return lorentzDrude(1, 10.83, 0.575, 0.030, {{0.061, 0.291, 0.378}, {0.104, 2.957, 1.056}, {0.723, 5.300, 3.213}, {0.638, 11.18, 4.305}},
                            extinction);
    return lorentzDrude(1, 14.98, 0.523, 0.047, {{0.227, 0.162, 0.333}, {0.050, 1.544, 0.312}, {0.166, 1.808, 1.351}, {0.030, 3.473, 3.382}},
                        extinction);
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
        // (Vurgaftman et al.'s bowings of the Γ gap, the split-off energy,
        // the electron's mass and the valence band's offset.)
        {"AlGaP", "AlP", "GaP", 0, 0, 0, 0, 0, 0, 0},
        {"AlInP", "AlP", "InP", -0.48, 0, 0, 0, 0, 0, 0},
        {"AlGaSb", "AlSb", "GaSb", -0.044, 1.22, 0, 0.3, 0, 0, 0},
        {"AlAsSb", "AlAs", "AlSb", 0.8, 0, -1.71, 0, 0, 0, 0},
        {"InAsSb", "InAs", "InSb", 0.67, 0, 0, 1.2, 0.035, 0, 0},
        {"InAlSb", "InSb", "AlSb", 0.43, 0, 0, 0.25, 0, 0, 0},
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
    } else if (compound.compare(QLatin1String("AlGaInP"), Qt::CaseInsensitive) == 0) {
        add("AlP", x);
        add("GaP", y);
        add("InP", 1 - x - y);
    }
    return out;
}

/// The model sets an alloy is of and their shares: its binaries' (the
/// nitrides' too).
std::vector<std::pair<const ModelSet*, double>> modelShares(const QString& compound, double x, double y)
{
    std::vector<std::pair<const ModelSet*, double>> out;
    for (const auto& [b, w] : shares(compound, x, y))
        if (const ModelSet* m = modelSet(QLatin1String(b->name))) out.emplace_back(m, w);
    if (!out.empty()) return out;
    auto add = [&](const char* name, double w) {
        if (w > 0)
            if (const ModelSet* m = modelSet(QLatin1String(name))) out.emplace_back(m, w);
    };
    const QString c = compound.trimmed();
    if (const ModelSet* m = modelSet(c)) out.emplace_back(m, 1.0);
    else if (c.compare(QLatin1String("AlGaN"), Qt::CaseInsensitive) == 0) {
        add("AlN", x);
        add("GaN", 1 - x);
    } else if (c.compare(QLatin1String("InGaN"), Qt::CaseInsensitive) == 0) {
        add("InN", x);
        add("GaN", 1 - x);
    } else if (c.compare(QLatin1String("InAlN"), Qt::CaseInsensitive) == 0) {
        add("InN", x);
        add("AlN", 1 - x);
    } else if (c.compare(QLatin1String("AlGaInN"), Qt::CaseInsensitive) == 0) {
        add("AlN", x);
        add("GaN", y);
        add("InN", 1 - x - y);
    } else if (c.compare(QLatin1String("SiGe"), Qt::CaseInsensitive) == 0) {
        add("Si", 1 - x);
        add("Ge", x);
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

/// \a e with \a props added or replaced.
MaterialEntry with(MaterialEntry e, std::initializer_list<std::pair<const char*, QString>> props)
{
    for (const auto& [k, v] : props) e.properties.insert(QString::fromLatin1(k), v);
    return e;
}

/// A (100) wafer's axes, the model's: its normal [001] up (y), [1-10]
/// across (x), [-1-10] along z - Euler's θ -90°, ψ 45°.
MaterialEntry onWafer(MaterialEntry e)
{
    return with(std::move(e), {{"orientation", QStringLiteral("euler")}, {"euler_theta", QStringLiteral("-90")}, {"euler_psi", QStringLiteral("45")}});
}

/// \a e with model set \a set's parameters (where it has none of its own).
MaterialEntry withModels(MaterialEntry e, const char* set)
{
    QJsonObject p;
    addModels(p, *modelSet(QLatin1String(set)));
    for (auto it = p.constBegin(); it != p.constEnd(); ++it)
        if (!e.properties.contains(it.key())) e.properties.insert(it.key(), it.value());
    return e;
}

/// A metal's work function (eV, the CRC's - Lumerical's too) and its
/// optical constants of the wavelength, held from \a range.
MaterialEntry metal(MaterialEntry e, const char* symbol, double phi, const char* range)
{
    return with(std::move(e), {{"phi", QStringLiteral("%1[eV]").arg(number(phi))},
                               {"n", metalOptics(symbol, false)},
                               {"kappa", metalOptics(symbol, true)},
                               {"nrange", QString::fromLatin1(range)}});
}

/// A III-V's entry: its compound (the band parameters, an alloy's at its
/// composition) and, a binary's, what the other physics read of it - its
/// gap Varshni's, its densities of states, mobilities and conductivity of
/// heat of the temperature T, its models' parameters.
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
    if (!x) {
        const QString c = QString::fromLatin1(compound);
        for (const char* key : {"epsilonr", "n", "kappa", "nrange", "k", "rho", "Cp", "mur", "Eg", "chi", "Nc", "Nv", "mun", "mup", "taun", "taup"}) {
            const QString v = qstrcmp(key, "mur") == 0 ? QStringLiteral("1") : compoundProperty(c, 0, 0, QString::fromLatin1(key));
            if (!v.isEmpty()) e.properties.insert(QString::fromLatin1(key), v);
        }
        // (Of the temperature: as compoundProperty has them at T.)
        if (const Binary* b = binary(c)) {
            e.properties.insert(QStringLiteral("Eg"), varshniOfT(binaryAt(*b, 300).Eg, b->alpha, b->beta));
            const ModelSet* m = modelSet(c);
            e.properties.insert(QStringLiteral("mun"), ofT(b->mun, QStringLiteral("cm^2/(V*s)"), m ? -m->zeta[0] : 0));
            e.properties.insert(QStringLiteral("mup"), ofT(b->mup, QStringLiteral("cm^2/(V*s)"), m ? -m->zeta[1] : 0));
            e.properties.insert(QStringLiteral("k"), ofT(b->k, QStringLiteral("W/(m*K)"), heatExponent(c)));
        } else if (const Nitride* n = nitride(c)) {
            e.properties.insert(QStringLiteral("Eg"), varshniOfT(nitrideAt(*n, 300).Eg, n->alpha, n->beta));
        }
        for (const char* key : {"Nc", "Nv"}) {
            const QString v = e.properties.value(QString::fromLatin1(key)).toString();
            if (!v.isEmpty()) e.properties.insert(QString::fromLatin1(key), v + QStringLiteral("*(T/300[K])^1.5"));
        }
        if (const ModelSet* m = modelSet(c)) e = withModels(std::move(e), m->name);
    }
    return e;
}

} // namespace

const QList<MaterialEntry>& builtinMaterials()
{
    static const QList<MaterialEntry> list = {
        // Metals.
        // (Their work functions, for gate and Schottky contacts, and their
        // optical constants of the wavelength, for light: see metalOptics.)
        metal(entry("Copper", "Metals",
                    "Annealed copper, the IACS reference; work function 4.65 eV; n and κ from 0.6 to 2 µm (Rakić et al.'s "
                    "Lorentz-Drude fit: n 0.47 at 1550 nm, Johnson and Christy's 0.72, Babar and Weaver's 0.19 - films differ)",
                    {{"epsilonr", "1"}, {"sigma", "5.998e7[S/m]"}, {"k", "400[W/(m*K)]"}, {"rho", "8960[kg/m^3]"}, {"Cp", "385[J/(kg*K)]"},
                     {"mur", "1"}, {"E", "110[GPa]"}, {"nu", "0.35"}, {"alpha", "17e-6[1/K]"}}),
              "Cu", 4.65, "0.6[um] 2[um]"),
        metal(entry("Aluminium", "Metals", "Pure aluminium; work function 4.28 eV; n and κ from 0.3 to 2 µm (Rakić's)",
                    {{"epsilonr", "1"}, {"sigma", "3.774e7[S/m]"}, {"k", "238[W/(m*K)]"}, {"rho", "2700[kg/m^3]"}, {"Cp", "900[J/(kg*K)]"},
                     {"mur", "1"}, {"E", "70[GPa]"}, {"nu", "0.33"}, {"alpha", "23e-6[1/K]"}}),
              "Al", 4.28, "0.3[um] 2[um]"),
        metal(entry("Gold", "Metals", "Pure gold; work function 5.1 eV; n and κ from 0.5 to 1.9 µm (Vial et al.'s fit to Johnson and Christy)",
                    {{"epsilonr", "1"}, {"sigma", "4.10e7[S/m]"}, {"k", "317[W/(m*K)]"}, {"rho", "19300[kg/m^3]"}, {"Cp", "129[J/(kg*K)]"},
                     {"mur", "1"}, {"E", "79[GPa]"}, {"nu", "0.44"}, {"alpha", "14.2e-6[1/K]"}}),
              "Au", 5.1, "0.5[um] 1.9[um]"),
        metal(entry("Silver", "Metals",
                    "Pure silver; work function 4.26 eV; n and κ from 0.4 to 25 µm (a Drude-Lorentz fit to Yang et al.'s, "
                    "template-stripped silver: within 3 % to 1 µm, 1 % beyond; Johnson and Christy's κ within 5 %)",
                    {{"epsilonr", "1"}, {"sigma", "6.30e7[S/m]"}, {"k", "429[W/(m*K)]"}, {"rho", "10490[kg/m^3]"}, {"Cp", "235[J/(kg*K)]"},
                     {"mur", "1"}, {"E", "83[GPa]"}, {"nu", "0.37"}, {"alpha", "18.9e-6[1/K]"}}),
              "Ag", 4.26, "0.4[um] 25[um]"),
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
        // Semiconductors and their insulators (no conductivity: it depends on
        // the doping). Their gaps, densities of states, mobilities, lifetimes
        // and conductivities of heat expressions of the temperature T (as at
        // 300 K there); their models' parameters each its own (ModelSet).
        onWafer(withModels(with(entry("Silicon", "Semiconductors",
                              "Crystalline silicon: at 300 K its gap 1.12 eV (Varshni's of T), its densities of states (T/300 K)^1.5, mobilities "
                              "(T/300 K)^-ζ, conductivity of heat (T/300 K)^-1.65; its models' parameters (Caughey and Thomas's, Masetti's, "
                              "saturation, Slotboom's, Fossum's, Auger, radiative, van Overstraeten and de Man's, Hurkx's); n and κ from 0.25 "
                              "to 14 µm (Green's measured n below 0.4 µm, above it a Sellmeier fitted to Green's and Li's, within 0.007 of "
                              "both; Green's absorption at 300 K, nought beyond 1.45 µm); a cubic crystal's stiffness (C11 165.7, C12 63.9, "
                              "C44 79.6 GPa: 169 GPa along x, a (100) wafer's [110]) and photoelastic constants (p11 -0.094, p12 0.017, p44 "
                              "-0.051, at 1550 nm), its axes a (100) wafer's - [001] up, [1-10] across, a ⟨110⟩ along z (its orientation). Its "
                              "conductivity depends on its doping: give it, or solve its carriers (Semiconductor)",
                              {{"epsilonr", "11.7"}, {"rho", "2329[kg/m^3]"}, {"Cp", "700[J/(kg*K)]"}, {"mur", "1"}, {"E", "170[GPa]"},
                               {"nu", "0.28"}, {"alpha", "2.6e-6[1/K]"}, {"dndT", "1.86e-4[1/K]"}, {"chi", "4.05[eV]"}, {"taun", "10[us]"},
                               {"taup", "10[us]"}, {"sigmaFCn", "8.5e-18[cm^2]"}, {"sigmaFCp", "6.0e-18[cm^2]"}}),
                        {{"Eg", varshniOfT(1.12, 4.73e-4, 636)}, {"Nc", ofT(2.8e19, QStringLiteral("1/cm^3"), 1.5)},
                         {"Nv", ofT(1.04e19, QStringLiteral("1/cm^3"), 1.5)}, {"mun", ofT(1400, QStringLiteral("cm^2/(V*s)"), -2.5)},
                         {"mup", ofT(450, QStringLiteral("cm^2/(V*s)"), -2.2)}, {"k", ofT(130, QStringLiteral("W/(m*K)"), -1.65)},
                         {"n", siliconIndex()}, {"kappa", siliconExtinction()}, {"nrange", QStringLiteral("0.25[um] 14[um]")},
                         // (Masetti's: arsenic's and boron's, the feature's defaults.)
                         {"m1n", QStringLiteral("52.2[cm^2/(V*s)]")}, {"m2n", QStringLiteral("52.2[cm^2/(V*s)]")},
                         {"mu1n", QStringLiteral("43.4[cm^2/(V*s)]")}, {"pcn", QStringLiteral("0[1/cm^3]")},
                         {"crn", QStringLiteral("9.68e16[1/cm^3]")}, {"csn", QStringLiteral("3.43e20[1/cm^3]")}, {"man", QStringLiteral("0.68")},
                         {"mbn", QStringLiteral("2")}, {"m1p", QStringLiteral("44.9[cm^2/(V*s)]")}, {"m2p", QStringLiteral("0[cm^2/(V*s)]")},
                         {"mu1p", QStringLiteral("29.0[cm^2/(V*s)]")}, {"pcp", QStringLiteral("9.23e16[1/cm^3]")},
                         {"crp", QStringLiteral("2.23e17[1/cm^3]")}, {"csp", QStringLiteral("6.1e20[1/cm^3]")}, {"map", QStringLiteral("0.719")},
                         {"mbp", QStringLiteral("2")},
                         // (Van Overstraeten and de Man's holes' second range, above 400 kV/cm.)
                         {"iian2", QStringLiteral("7.03e5[1/cm]")}, {"iibn2", QStringLiteral("1.231e6[V/cm]")},
                         {"iiap2", QStringLiteral("6.71e5[1/cm]")}, {"iibp2", QStringLiteral("1.693e6[V/cm]")}, {"iiE0", QStringLiteral("4e5[V/cm]")},
                         // (Hall's stiffness; the photoelastic constants as used near 1550 nm.)
                         {"C", cubicStiffness(165.7, 63.9, 79.6)}, {"p", cubicPhotoelastic(-0.094, 0.017, -0.051)}}),
                   "Si")),
        withModels(with(entry("Germanium", "Semiconductors",
                              "Crystalline germanium: at 300 K its gap 0.66 eV (Varshni's of T), its densities of states, mobilities and "
                              "conductivity of heat of T; its models' parameters (Lumerical's); n and κ from 0.3 to 14 µm (Nunley et al.'s "
                              "measured to 1.9 µm - its direct gap's absorption, 2000 /cm at 1550 nm -, Burnett et al.'s Sellmeier from 2 µm). "
                              "Its direct gap for a Wave Equation's Direct-gap absorption (the Franz-Keldysh effect; κ then the option's): "
                              "Egd 0.80 eV, A 2e4 /cm (8000 /cm at 1310 nm), µ 0.03",
                              {{"epsilonr", "16.2"}, {"rho", "5323[kg/m^3]"}, {"Cp", "320[J/(kg*K)]"}, {"mur", "1"}, {"E", "103[GPa]"},
                               {"nu", "0.26"}, {"alpha", "5.9e-6[1/K]"}, {"nrange", "0.3[um] 14[um]"}, {"dndT", "4.2e-4[1/K]"},
                               {"chi", "4.0[eV]"}, {"taun", "1[ms]"}, {"taup", "1[ms]"}, {"Egd", "0.80[eV]"}, {"mr", "0.03"}, {"Afk", "2e4[1/cm]"}}),
                        {{"Eg", varshniOfT(0.66, 4.774e-4, 235)}, {"n", germaniumIndex()}, {"kappa", germaniumExtinction()},
                         {"Nc", ofT(1.04e19, QStringLiteral("1/cm^3"), 1.5)},
                         {"Nv", ofT(6.0e18, QStringLiteral("1/cm^3"), 1.5)}, {"mun", ofT(3900, QStringLiteral("cm^2/(V*s)"), -1.66)},
                         {"mup", ofT(1900, QStringLiteral("cm^2/(V*s)"), -2.33)}, {"k", ofT(60, QStringLiteral("W/(m*K)"), -1.25)}}),
                   "Ge"),
        withModels(with(entry("Germanium on silicon", "Semiconductors",
                              "Germanium grown on silicon: 0.25 % tensile strain as it cools, its gaps narrowed - the direct one to 0.773 eV, "
                              "absorbing 4000 /cm at 1550 nm (a waveguide photodiode's): κ its direct gap's edge (Egd, A) or bulk "
                              "germanium's, the larger; n germanium's, 27 meV lower with the gap (its peak there, 1.60 µm). Its "
                              "lifetimes the threading dislocations'; of T as Germanium",
                              {{"epsilonr", "16.2"}, {"rho", "5323[kg/m^3]"}, {"Cp", "320[J/(kg*K)]"}, {"mur", "1"}, {"E", "103[GPa]"},
                               {"nu", "0.26"}, {"alpha", "5.9e-6[1/K]"}, {"nrange", "0.3[um] 14[um]"}, {"dndT", "4.2e-4[1/K]"},
                               {"chi", "4.0[eV]"}, {"taun", "1[ns]"}, {"taup", "1[ns]"}, {"Egd", "0.773[eV]"}, {"mr", "0.03"}, {"Afk", "2e4[1/cm]"}}),
                        {{"Eg", varshniOfT(0.65, 4.774e-4, 235)},
                         // (Bulk germanium's, its spectrum moved 27 meV down with
                         // its direct gap: the index's peak at the gap there.)
                         {"n", germaniumIndex().replace(QStringLiteral("lambda0"), QStringLiteral("(lambda0/(1 + 0.027*lambda0/1.23984198[um]))"))},
                         {"kappa", QStringLiteral("max(%1, %2)").arg(edgeExtinction(2e4, 0.773), germaniumExtinction())},
                         {"Nc", ofT(1.04e19, QStringLiteral("1/cm^3"), 1.5)},
                         {"Nv", ofT(6.0e18, QStringLiteral("1/cm^3"), 1.5)}, {"mun", ofT(3900, QStringLiteral("cm^2/(V*s)"), -1.66)},
                         {"mup", ofT(1900, QStringLiteral("cm^2/(V*s)"), -2.33)}, {"k", ofT(60, QStringLiteral("W/(m*K)"), -1.25)}}),
                   "Ge"),
        withModels(with(entry("Gallium nitride", "Semiconductors",
                              "Wurtzite GaN: its bands for quantum wells (k·p 6x6, its polarization) its compound's; its gap 3.4 eV at 300 K "
                              "(Varshni's of T), its models' parameters Lumerical's; n from 0.37 to 10 µm, the ordinary ray's - across its c "
                              "axis, a c-plane film's in its plane - (Barker and Ilegems's), held beyond",
                              {{"compound", "GaN"}, {"epsilonr", "8.9"}, {"k", "130[W/(m*K)]"}, {"rho", "6150[kg/m^3]"}, {"Cp", "490[J/(kg*K)]"},
                               {"mur", "1"}, {"E", "295[GPa]"}, {"nu", "0.23"}, {"alpha", "5.6e-6[1/K]"},
                               {"nrange", "0.37[um] 10[um]"}, {"chi", "4.1[eV]"}, {"taun", "1[ns]"}, {"taup", "1[ns]"}}),
                        {{"Eg", varshniOfT(3.4, 0.909e-3, 830)}, {"n", heldSellmeier(3.60, {{1.75, 0.256}, {4.1, 17.86}}, 0.37, 10)},
                         {"Nc", ofT(2.3e18, QStringLiteral("1/cm^3"), 1.5)},
                         {"Nv", ofT(4.6e19, QStringLiteral("1/cm^3"), 1.5)}, {"mun", ofT(1000, QStringLiteral("cm^2/(V*s)"), -1.26)},
                         {"mup", ofT(30, QStringLiteral("cm^2/(V*s)"), -2)}}),
                   "GaN"),
        with(entry("Silicon dioxide", "Semiconductors",
                   "SiO₂, thermal oxide or fused silica; n from 0.21 to 6.7 µm (Malitson's - Fused silica's too, which has the "
                   "glass's figures of heat and stiffness); κ in the infrared, from 2.5 µm (Franta et al.'s fused silica: 0.0048 at "
                   "5 µm)",
                   {{"epsilonr", "3.9"}, {"tand", "0.0001"}, {"sigma", "1e-15[S/m]"}, {"k", "1.4[W/(m*K)]"}, {"rho", "2200[kg/m^3]"},
                    {"Cp", "730[J/(kg*K)]"}, {"mur", "1"}, {"E", "70[GPa]"}, {"nu", "0.17"}, {"alpha", "0.5e-6[1/K]"}, {"dndT", "9.5e-6[1/K]"},
                    {"nrange", "0.21[um] 6.7[um]"}}),
             {{"n", sellmeier(1, {{0.6961663, 0.0684043}, {0.4079426, 0.1162414}, {0.8974794, 9.896161}})}, {"kappa", silicaExtinction()},
              // (Fused silica's photoelastic constants, an isotropic solid's.)
              {"p", cubicPhotoelastic(0.121, 0.270, (0.121 - 0.270) / 2)}}),
        with(entry("Silicon nitride", "Semiconductors",
                   "Si₃N₄, stoichiometric (a thin film conducts heat less); n from 0.31 to 5.5 µm (Luke et al.'s, an LPCVD film: "
                   "1.996 at 1550 nm; PECVD films 1.88-2.05 by their recipe - Silicon nitride (PECVD) -, sputtered ones to 2.46)",
                   {{"epsilonr", "7.5"}, {"sigma", "1e-14[S/m]"}, {"k", "20[W/(m*K)]"}, {"rho", "3100[kg/m^3]"}, {"Cp", "700[J/(kg*K)]"},
                    {"mur", "1"}, {"E", "250[GPa]"}, {"nu", "0.23"}, {"alpha", "2.3e-6[1/K]"}, {"dndT", "2.45e-5[1/K]"},
                    {"nrange", "0.31[um] 5.5[um]"}}),
             {{"n", sellmeier(1, {{3.0249, 0.1353406}, {40314, 1239.842}})}}),
        with(entry("Silicon nitride (PECVD)", "Semiconductors",
                   "SiNₓ deposited by PECVD: n from 0.3 to 1.69 µm (a Sellmeier fitted to Beliaev et al.'s film, within 0.007: 1.990 at "
                   "1550 nm - recipes vary it, 1.88-2.05, Vogt's films); its other figures stoichiometric Si₃N₄'s",
                   {{"epsilonr", "7"}, {"sigma", "1e-14[S/m]"}, {"k", "20[W/(m*K)]"}, {"rho", "3100[kg/m^3]"}, {"Cp", "700[J/(kg*K)]"},
                    {"mur", "1"}, {"E", "250[GPa]"}, {"nu", "0.23"}, {"alpha", "2.3e-6[1/K]"}, {"dndT", "2.45e-5[1/K]"},
                    {"nrange", "0.3[um] 1.69[um]"}}),
             {{"n", sellmeier(1, {{2.93502, 0.13673}})}}),
        // Gate and capacitor dielectrics (Robertson's permittivities, as
        // Lumerical's; their heat Lumerical's, a bulk crystal's).
        with(entry("Hafnium oxide", "Semiconductors",
                   "HfO₂, a high-κ gate dielectric (εr 25, Robertson's); n from 0.2 to 2 µm, an e-beam film's (Al-Kuhaili's Cauchy: "
                   "1.878 at 1550 nm; sputtered films 1.96 - Hafnium oxide (sputtered) -, others to 2.05), held beyond",
                   {{"epsilonr", "25"}, {"sigma", "1e-15[S/m]"}, {"k", "22[W/(m*K)]"}, {"rho", "9680[kg/m^3]"}, {"Cp", "261[J/(kg*K)]"},
                    {"mur", "1"}, {"nrange", "0.2[um] 2[um]"}}),
             {{"n", QStringLiteral("1.875 + 6.28e-3[um^2]/%1^2 + 5.80e-4[um^4]/%1^4").arg(QStringLiteral("min(max(lambda0, 0.2[um]), 2[um])"))}}),
        with(entry("Hafnium oxide (sputtered)", "Semiconductors",
                   "HfO₂ sputtered: n from 0.4 to 5 µm (a Sellmeier fitted to Bright et al.'s films, within 0.0033: 1.957 at 1550 nm; "
                   "κ 0.0026 at 5 µm, rising: its phonons); its other figures Hafnium oxide's",
                   {{"epsilonr", "25"}, {"sigma", "1e-15[S/m]"}, {"k", "22[W/(m*K)]"}, {"rho", "9680[kg/m^3]"}, {"Cp", "261[J/(kg*K)]"},
                    {"mur", "1"}, {"nrange", "0.4[um] 5[um]"}}),
             {{"n", sellmeier(1, {{2.83267, 0.144922}, {188746, 4005.28}})}}),
        with(entry("Zirconium oxide", "Semiconductors", "ZrO₂, a high-κ gate dielectric (εr 24); n from 0.36 to 5.1 µm (Wood and Nassau's, cubic)",
                   {{"epsilonr", "24"}, {"sigma", "1e-15[S/m]"}, {"k", "2[W/(m*K)]"}, {"rho", "5680[kg/m^3]"}, {"Cp", "450[J/(kg*K)]"},
                    {"mur", "1"}, {"nrange", "0.36[um] 5.1[um]"}}),
             {{"n", sellmeier(1, {{1.347091, 0.062543}, {2.117788, 0.166739}, {9.452943, 24.320570}})}}),
        with(entry("Titanium oxide", "Semiconductors",
                   "TiO₂, rutile-like (εr 80, Robertson's); n from 0.43 to 1.53 µm, a rutile crystal's ordinary ray (DeVore's: 2.45 "
                   "at 1.5 µm; amorphous and anatase films 0.2-0.6 lower in the visible)",
                   {{"epsilonr", "80"}, {"sigma", "1e-15[S/m]"}, {"k", "7.4[W/(m*K)]"}, {"rho", "4250[kg/m^3]"}, {"Cp", "690[J/(kg*K)]"},
                    {"mur", "1"}, {"nrange", "0.43[um] 1.53[um]"}}),
             {{"n", QStringLiteral("sqrt(5.913 + 0.2441[um^2]/(lambda0^2 - 0.0803[um^2]))")}}),
        with(entry("Aluminium oxide", "Semiconductors",
                   "Al₂O₃ as a gate dielectric (εr 9, Robertson's); n from 0.2 to 5 µm, a crystal's - sapphire's ordinary ray "
                   "(Malitson and Dodge's): 1.746 at 1550 nm; amorphous and ALD films 1.61-1.66 - Aluminium oxide (ALD)",
                   {{"epsilonr", "9"}, {"sigma", "1e-15[S/m]"}, {"k", "28[W/(m*K)]"}, {"rho", "3900[kg/m^3]"}, {"Cp", "796[J/(kg*K)]"},
                    {"mur", "1"}, {"nrange", "0.2[um] 5[um]"}}),
             {{"n", sellmeier(1, {{1.4313493, 0.0726631}, {0.65054713, 0.1193242}, {5.3414021, 18.028251}})}}),
        with(entry("Aluminium oxide (ALD)", "Semiconductors",
                   "Al₂O₃ deposited by (plasma-enhanced) atomic layers: n from 0.3 to 5 µm (a Sellmeier fitted to Rodriguez de Marcos "
                   "et al.'s, within 0.001: 1.646 at 1550 nm; κ 0.0018 at 4 µm, rising: its phonons); its other figures Aluminium "
                   "oxide's",
                   {{"epsilonr", "9"}, {"sigma", "1e-15[S/m]"}, {"k", "28[W/(m*K)]"}, {"rho", "3900[kg/m^3]"}, {"Cp", "796[J/(kg*K)]"},
                    {"mur", "1"}, {"nrange", "0.3[um] 5[um]"}}),
             {{"n", sellmeier(1, {{1.74187, 0.100569}, {3.51452, 14.5533}})}}),
        onWafer(withModels(with(entry("Indium phosphide", "Semiconductors",
                              "InP: its gap 1.344 eV at 300 K (Varshni's of T), its models' parameters Lumerical's; n from 0.95 to 10 µm "
                              "(Pettit and Turner's); Pockels' r41 1.45 pm/V, its axes a (100) wafer's (its orientation); its edge's "
                              "absorption A 1.16e5 per cm (Aspnes and Studna's k from 1.5 to 1.7 eV, within 4 %)",
                              {{"compound", "InP"}, {"epsilonr", "12.4"}, {"rho", "4810[kg/m^3]"}, {"Cp", "310[J/(kg*K)]"}, {"mur", "1"},
                               {"Afk", "1.16e5[1/cm]"},
                               {"E", "61[GPa]"}, {"nu", "0.36"}, {"alpha", "4.6e-6[1/K]"}, {"dndT", "2.0e-4[1/K]"},
                               {"chi", "4.38[eV]"}, {"taun", "1[ns]"}, {"taup", "1[ns]"}, {"sigmaFCn", "1e-18[cm^2]"}, {"sigmaFCp", "2e-17[cm^2]"}}),
                        {{"Eg", varshniOfT(1.344, 0.363e-3, 162)}, {"Nc", ofT(5.7e17, QStringLiteral("1/cm^3"), 1.5)},
                         {"Nv", ofT(1.1e19, QStringLiteral("1/cm^3"), 1.5)}, {"mun", ofT(5400, QStringLiteral("cm^2/(V*s)"), -1.9)},
                         {"mup", ofT(200, QStringLiteral("cm^2/(V*s)"), -1.2)}, {"k", ofT(68, QStringLiteral("W/(m*K)"), -1.4)},
                         {"n", compoundProperty(QStringLiteral("InP"), 0, 0, QStringLiteral("n"))},
                         {"kappa", compoundProperty(QStringLiteral("InP"), 0, 0, QStringLiteral("kappa"))},
                         {"nrange", compoundProperty(QStringLiteral("InP"), 0, 0, QStringLiteral("nrange"))}, {"r", zincBlendeElectroOptic(1.45)}}),
                   "InP")),
        onWafer(withModels(with(entry("Gallium arsenide", "Semiconductors",
                              "GaAs: its gap 1.424 eV at 300 K (Varshni's of T), its models' parameters Lumerical's; n from 0.88 to 5 µm "
                              "(Afromowitz's); Pockels' r41 1.5 pm/V, its axes a (100) wafer's (its orientation)",
                              {{"compound", "GaAs"}, {"epsilonr", "12.9"}, {"rho", "5320[kg/m^3]"}, {"Cp", "330[J/(kg*K)]"}, {"mur", "1"},
                               {"E", "85.9[GPa]"}, {"nu", "0.31"}, {"alpha", "5.73e-6[1/K]"}, {"dndT", "2.35e-4[1/K]"},
                               {"chi", "4.07[eV]"}}),
                        {{"Eg", varshniOfT(1.424, 0.5405e-3, 204)}, {"Nc", ofT(4.7e17, QStringLiteral("1/cm^3"), 1.5)},
                         {"Nv", ofT(9.0e18, QStringLiteral("1/cm^3"), 1.5)}, {"mun", ofT(8500, QStringLiteral("cm^2/(V*s)"), -2.2)},
                         {"mup", ofT(400, QStringLiteral("cm^2/(V*s)"), -0.9)}, {"k", ofT(55, QStringLiteral("W/(m*K)"), -1.25)},
                         {"taun", ofT(5, QStringLiteral("ns"), -0.5)}, {"taup", ofT(5, QStringLiteral("ns"), -0.5)},
                         {"n", compoundProperty(QStringLiteral("GaAs"), 0, 0, QStringLiteral("n"))},
                         {"kappa", compoundProperty(QStringLiteral("GaAs"), 0, 0, QStringLiteral("kappa"))},
                         {"nrange", compoundProperty(QStringLiteral("GaAs"), 0, 0, QStringLiteral("nrange"))},
                         {"r", zincBlendeElectroOptic(1.5)}}),
                   "GaAs")),
        // II-VI semiconductors (thin-film solar cells): their gaps measured at
        // 300 K, constant; χ from Lumerical's work functions, Nc and Nv of its
        // masses, its models' parameters; the holes' mobilities Sze's (CdSe's
        // an estimate); n of the wavelength, held beyond where measured.
        withModels(with(entry("Cadmium sulfide", "Semiconductors",
                              "CdS: a window layer (2.42 eV at 300 K); its lifetimes a crystal's (a film's are nanoseconds); n from 0.52 to "
                              "1.4 µm, the ordinary ray's (Bieniewski and Czyzak's)",
                              {{"epsilonr", "10"}, {"k", "20[W/(m*K)]"}, {"rho", "4826[kg/m^3]"}, {"Cp", "470[J/(kg*K)]"}, {"mur", "1"},
                               {"nrange", "0.52[um] 1.4[um]"}, {"Eg", "2.42[eV]"}, {"chi", "4.82[eV]"}, {"taun", "1[us]"},
                               {"taup", "1[us]"}}),
                        {{"n", QStringLiteral("sqrt(5.1792 + 0.23504[um^2]/(%1^2 - 0.083591[um^2]) + 0.036927[um^2]/(%1^2 - 0.23504[um^2]))")
                                   .arg(QStringLiteral("min(max(lambda0, 0.52[um]), 1.4[um])"))},
                         {"Nc", ofT(2.4145e18, QStringLiteral("1/cm^3"), 1.5)}, {"Nv", ofT(2.5657e19, QStringLiteral("1/cm^3"), 1.5)},
                         {"mun", QStringLiteral("340[cm^2/(V*s)]")}, {"mup", QStringLiteral("50[cm^2/(V*s)]")}}),
                   "CdS"),
        withModels(with(entry("Cadmium selenide", "Semiconductors",
                              "CdSe (1.74 eV at 300 K); its hole mobility an estimate; n from 1.01 to 22 µm, the ordinary ray's (Lisitsa "
                              "et al.'s)",
                              {{"epsilonr", "10"}, {"k", "9[W/(m*K)]"}, {"rho", "5816[kg/m^3]"}, {"Cp", "490[J/(kg*K)]"}, {"mur", "1"},
                               {"nrange", "1.01[um] 22[um]"}, {"Eg", "1.74[eV]"}, {"chi", "5.00[eV]"}, {"taun", "1[us]"},
                               {"taup", "1[us]"}}),
                        {{"n", heldSellmeier(4.2243, {{1.768, std::sqrt(0.227)}, {3.12, std::sqrt(3380.0)}}, 1.01, 22)},
                         {"Nc", ofT(1.1760e18, QStringLiteral("1/cm^3"), 1.5)}, {"Nv", ofT(2.8946e19, QStringLiteral("1/cm^3"), 1.5)},
                         {"mun", QStringLiteral("650[cm^2/(V*s)]")}, {"mup", QStringLiteral("50[cm^2/(V*s)]")}}),
                   "CdSe"),
        withModels(with(entry("Cadmium telluride", "Semiconductors",
                              "CdTe: a solar absorber (1.50 eV at 300 K); its lifetimes a crystal's (a film's are nanoseconds); n from 0.86 "
                              "to 2.5 µm (Marple's)",
                              {{"epsilonr", "10.2"}, {"k", "6.2[W/(m*K)]"}, {"rho", "5850[kg/m^3]"}, {"Cp", "210[J/(kg*K)]"}, {"mur", "1"},
                               {"nrange", "0.86[um] 2.5[um]"}, {"Eg", "1.50[eV]"}, {"chi", "4.32[eV]"}, {"taun", "1[us]"},
                               {"taup", "1[us]"}}),
                        {{"n", heldSellmeier(5.68, {{1.53, std::sqrt(0.366)}}, 0.86, 2.5)},
                         {"Nc", ofT(9.1535e17, QStringLiteral("1/cm^3"), 1.5)}, {"Nv", ofT(1.6623e19, QStringLiteral("1/cm^3"), 1.5)},
                         {"mun", QStringLiteral("1050[cm^2/(V*s)]")}, {"mup", QStringLiteral("100[cm^2/(V*s)]")}}),
                   "CdTe"),
        // Silicon germanium: x the germanium's share - its X and L valleys'
        // gaps (Lumerical's bowing of the X one), the lower; mobilities by
        // harmonic means with the alloy's scattering.
        with(entry("Silicon germanium", "Semiconductors",
                   "Si₁₋ₓGeₓ, x the germanium's share (HBT bases, strained channels, photodetectors): its gap the lower of its X and L "
                   "valleys' (the L one below from x 0.8), its mobilities scattered by the alloy",
                   {{"compound", "SiGe"}, {"x", "0.2"}, {"mur", "1"}}),
             {}),
        // III-V binaries and alloys: their band parameters for quantum wells
        // (an alloy's at its composition x, y), the rest rough.
        compoundEntry("Aluminium arsenide", "AlAs: indirect, its Γ gap 3.0 eV at 300 K", "AlAs"),
        compoundEntry("Aluminium phosphide", "AlP: indirect, its Γ gap 3.55 eV at 300 K (its X one 2.49)", "AlP"),
        compoundEntry("Indium arsenide", "InAs: a narrow gap, 0.35 eV", "InAs"),
        compoundEntry("Gallium phosphide", "GaP: indirect, its Γ gap 2.78 eV", "GaP"),
        compoundEntry("Gallium antimonide", "GaSb", "GaSb"),
        compoundEntry("Aluminium antimonide", "AlSb: indirect, its Γ gap 2.3 eV", "AlSb"),
        compoundEntry("Indium antimonide", "InSb: the narrowest gap, 0.17 eV", "InSb"),
        compoundEntry("Aluminium gallium arsenide", "AlₓGa₁₋ₓAs, x the aluminium's share (barriers to GaAs; direct below x 0.45)", "AlGaAs", "0.3"),
        with(compoundEntry("Indium gallium arsenide", "InₓGa₁₋ₓAs, x the indium's share: 0.53 is InP's lattice, 0.2 a strained well on GaAs",
                      "InGaAs", "0.53"), {{"sigmaFCn", QStringLiteral("1e-18[cm^2]")}, {"sigmaFCp", QStringLiteral("2e-17[cm^2]")}}),
        with(compoundEntry("Indium aluminium arsenide", "InₓAl₁₋ₓAs, x the indium's share: 0.52 is InP's lattice", "InAlAs", "0.52"), {{"sigmaFCn", QStringLiteral("1e-18[cm^2]")}, {"sigmaFCp", QStringLiteral("2e-17[cm^2]")}}),
        compoundEntry("Gallium indium phosphide", "GaₓIn₁₋ₓP, x the gallium's share: 0.51 is GaAs's lattice", "GaInP", "0.51"),
        compoundEntry("Aluminium indium phosphide", "AlₓIn₁₋ₓP, x the aluminium's share: 0.52 is GaAs's lattice (visible lasers' cladding)",
                      "AlInP", "0.52"),
        compoundEntry("Aluminium gallium phosphide", "AlₓGa₁₋ₓP, x the aluminium's share", "AlGaP", "0.5"),
        compoundEntry("Aluminium gallium antimonide", "AlₓGa₁₋ₓSb, x the aluminium's share", "AlGaSb", "0.3"),
        compoundEntry("Aluminium arsenide antimonide", "AlAsₓSb₁₋ₓ, x the arsenic's share: 0.56 is InP's lattice", "AlAsSb", "0.56"),
        compoundEntry("Indium arsenide antimonide", "InAsₓSb₁₋ₓ, x the arsenic's share: 0.91 is GaSb's lattice (mid-infrared)", "InAsSb", "0.91"),
        compoundEntry("Indium aluminium antimonide", "InₓAl₁₋ₓSb, x the indium's share", "InAlSb", "0.9"),
        with(compoundEntry("Indium gallium arsenide phosphide", "In₁₋ₓGaₓAsᵧP₁₋ᵧ: on InP's lattice y ≈ 2.2 x; x 0.28, y 0.6 is the 1.3 µm quaternary",
                      "InGaAsP", "0.28", "0.6"), {{"sigmaFCn", QStringLiteral("1e-18[cm^2]")}, {"sigmaFCp", QStringLiteral("2e-17[cm^2]")}}),
        with(compoundEntry("Aluminium gallium indium arsenide", "AlₓGaᵧIn₁₋ₓ₋ᵧAs: x + y 0.47 is InP's lattice", "AlGaInAs", "0.2", "0.27"), {{"sigmaFCn", QStringLiteral("1e-18[cm^2]")}, {"sigmaFCp", QStringLiteral("2e-17[cm^2]")}}),
        compoundEntry("Aluminium gallium indium phosphide",
                      "AlₓGaᵧIn₁₋ₓ₋ᵧP: x + y 0.51 is GaAs's lattice - (Al₀.₃Ga₀.₇)₀.₅₁In₀.₄₉P, a red laser's barrier, is x 0.153, y 0.357",
                      "AlGaInP", "0.153", "0.357"),
        // The nitrides (wurtzite): their bands, strain and polarization for
        // quantum wells.
        compoundEntry("Aluminium nitride", "AlN: wurtzite, the widest gap (6.2 eV at 300 K); its crystal field below 0 - its top band Z-like, "
                      "its light TM", "AlN"),
        compoundEntry("Indium nitride", "InN: wurtzite, a narrow gap (0.76 eV)", "InN"),
        compoundEntry("Aluminium gallium nitride", "AlₓGa₁₋ₓN, x the aluminium's share (barriers to GaN; a bowing of 0.8 eV)", "AlGaN", "0.2"),
        compoundEntry("Indium gallium nitride", "InₓGa₁₋ₓN, x the indium's share (blue and green wells on GaN; a bowing of 1.4 eV)", "InGaN",
                      "0.15"),
        compoundEntry("Indium aluminium nitride", "InₓAl₁₋ₓN, x the indium's share: 0.18 is GaN's lattice", "InAlN", "0.18"),
        compoundEntry("Aluminium gallium indium nitride", "AlₓGaᵧIn₁₋ₓ₋ᵧN: its gap from its three ternaries' (Vurgaftman and Meyer's)",
                      "AlGaInN", "0.1", "0.8"),
        // Crystals: their tensors in their own axes (x1 a, x3 c), turned by
        // their orientation - lithium niobate's and tantalate's an x-cut
        // wafer's, y along the waveguide, their optic axis across (x).
        with(entry("Lithium niobate", "Crystals",
                   "LiNbO₃, congruent (3m), an x-cut wafer's, y-propagating (its orientation): εr 44 across its optic axis, 28 along it "
                   "(clamped); no and ne from 0.4 to 5 µm (Zelmon et al.'s) - no along its x1 and x2, across its optic axis, ne along x3, "
                   "the axis; Pockels' r13 8.6, r22 3.4, r33 30.8, r51 28 pm/V "
                   "(clamped); piezoelectric d15 69.2, d22 20.8, d31 -0.85, d33 6.0 pC/N and its stiffness (Warner et al.'s); its "
                   "photoelastic constants and expansion along its axes (Weis and Gaylord's summary), its conduction of heat 4.19 "
                   "W/(m·K) across its optic axis, 4.6 along it; dn/dT the extraordinary ray's",
                   {{"epsilonr", "44; 44; 28"}, {"sigma", "1e-15[S/m]"}, {"k", "4.19[W/(m*K)]; 4.19[W/(m*K)]; 4.6[W/(m*K)]"},
                    {"rho", "4640[kg/m^3]"}, {"Cp", "630[J/(kg*K)]"}, {"mur", "1"}, {"E", "170[GPa]"}, {"nu", "0.25"},
                    {"alpha", "15.4e-6[1/K]; 15.4e-6[1/K]; 7.5e-6[1/K]"}, {"dndT", "3.3e-5[1/K]"}, {"nrange", "0.4[um] 5[um]"},
                    {"orientation", "cut"}, {"cut", "x"}, {"along", "y"}}),
             {{"n", sellmeier(1, {{2.6734, std::sqrt(0.01764)}, {1.2290, std::sqrt(0.05914)}, {12.614, std::sqrt(474.60)}}) + QStringLiteral("; ") +
                        sellmeier(1, {{2.6734, std::sqrt(0.01764)}, {1.2290, std::sqrt(0.05914)}, {12.614, std::sqrt(474.60)}}) + QStringLiteral("; ") +
                        sellmeier(1, {{2.9804, std::sqrt(0.02047)}, {0.5981, std::sqrt(0.0666)}, {8.9543, std::sqrt(416.08)}})},
              {"r", trigonal3mElectroOptic(8.6, 3.4, 30.8, 28)},
              {"d", trigonal3mStrainPiezo(69.2, 20.8, -0.85, 6.0)},
              {"C", trigonalStiffness(203, 53, 75, 9, 245, 60)},
              {"p", trigonalPhotoelastic(-0.026, 0.090, 0.133, -0.075, 0.179, 0.071, -0.151, 0.146)}}),
        with(entry("Lithium tantalate", "Crystals",
                   "LiTaO₃, congruent (3m), an x-cut wafer's, y-propagating (its orientation): εr 41 across its optic axis, 43 along it "
                   "(clamped); no and ne from 0.45 to 4 µm (Sellmeiers fitted to Bond's measured indices, within 0.0007) - no along its "
                   "x1 and x2, across its optic axis, ne along x3, the axis; Pockels' r13 7.5, r22 1, r33 33, r51 20 pm/V "
                   "(clamped; its sources differ, r33 30 to 36); piezoelectric d15 26, d22 8.5, d31 -3.0, d33 9.2 pC/N and its stiffness "
                   "(Warner et al.'s)",
                   {{"epsilonr", "41; 41; 43"}, {"sigma", "1e-15[S/m]"}, {"k", "4.6[W/(m*K)]"}, {"rho", "7454[kg/m^3]"}, {"Cp", "424[J/(kg*K)]"},
                    {"mur", "1"}, {"alpha", "16.1e-6[1/K]; 16.1e-6[1/K]; 4.1e-6[1/K]"}, {"nrange", "0.45[um] 4[um]"}, {"orientation", "cut"},
                    {"cut", "x"}, {"along", "y"}}),
             {{"n", sellmeier(2.23997, {{2.27035, 0.194489}, {18.708, 28.2899}}) + QStringLiteral("; ") +
                        sellmeier(2.23997, {{2.27035, 0.194489}, {18.708, 28.2899}}) + QStringLiteral("; ") +
                        sellmeier(2.18189, {{2.34332, 0.192952}, {7.17944, 17.8713}})},
              {"r", trigonal3mElectroOptic(7.5, 1, 33, 20)},
              {"d", trigonal3mStrainPiezo(26, 8.5, -3.0, 9.2)},
              {"C", trigonalStiffness(233, 47, 80, -11, 275, 94)}}),
        with(entry("KTP", "Crystals",
                   "KTiOPO₄ (mm2), its axes the model's: nx, ny, nz from 0.43 to 3.54 µm (Kato and Takaoka's); Pockels' r13 9.5, r23 15.7, "
                   "r33 36.3, r42 9.3, r51 7.3 pm/V; its expansion and conduction of heat along its axes; εr a datasheet's effective 13",
                   {{"epsilonr", "13"}, {"sigma", "1e-12[S/m]"}, {"k", "2[W/(m*K)]; 3[W/(m*K)]; 3.3[W/(m*K)]"}, {"rho", "3010[kg/m^3]"},
                    {"Cp", "728[J/(kg*K)]"}, {"mur", "1"}, {"alpha", "11e-6[1/K]; 9e-6[1/K]; 0.6e-6[1/K]"}, {"nrange", "0.43[um] 3.54[um]"}}),
             {{"n", katoTakaoka(3.29100, 0.04140, 0.03978, 9.35522, 31.45571) + QStringLiteral("; ") +
                        katoTakaoka(3.45018, 0.04341, 0.04597, 16.98825, 39.43799) + QStringLiteral("; ") +
                        katoTakaoka(4.59423, 0.06206, 0.04763, 110.80672, 86.12171)},
              {"r", mm2ElectroOptic(9.5, 15.7, 36.3, 9.3, 7.3)}}),
        with(entry("Quartz", "Crystals",
                   "α-quartz (32), its axes the model's: εr 4.52 across its optic axis, 4.68 along it; no and ne from 0.2 to 2 µm (Ghosh's) - "
                   "no along x and y, ne along z, its optic axis; "
                   "piezoelectric d11 2.31, d14 0.727 pC/N and its stiffness (Bechmann's); its expansion and conduction of heat along its axes",
                   {{"epsilonr", "4.52; 4.52; 4.68"}, {"tand", "0.0001"}, {"sigma", "1e-16[S/m]"}, {"k", "6.2[W/(m*K)]; 6.2[W/(m*K)]; 10.4[W/(m*K)]"},
                    {"rho", "2648[kg/m^3]"}, {"Cp", "740[J/(kg*K)]"}, {"mur", "1"}, {"alpha", "13.4e-6[1/K]; 13.4e-6[1/K]; 8.0e-6[1/K]"},
                    {"nrange", "0.2[um] 2.05[um]"}}),
             {{"n", sellmeier(1.28604141, {{1.07044083, std::sqrt(1.00585997e-2)}, {1.10202242, 10}}) + QStringLiteral("; ") +
                        sellmeier(1.28604141, {{1.07044083, std::sqrt(1.00585997e-2)}, {1.10202242, 10}}) + QStringLiteral("; ") +
                        sellmeier(1.28851804, {{1.09509924, std::sqrt(1.02101864e-2)}, {1.15662475, 10}})},
              {"d", trigonal32StrainPiezo(2.31, 0.727)},
              {"C", trigonalStiffness(86.74, 6.99, 11.91, -17.91, 107.2, 57.94)}}),
        with(entry("Sapphire", "Crystals",
                   "Al₂O₃ single crystal (-3m), a c-plane wafer's (its orientation: its optic axis up): εr 9.4 across its optic axis, 11.6 "
                   "along it; no and ne from 0.2 to 5 µm (Malitson and Dodge's) - no across the optic axis (x1, x2), ne along it (x3); its "
                   "stiffness (Wachtman et al.'s); its expansion and "
                   "conduction of heat along its axes",
                   {{"epsilonr", "9.4; 9.4; 11.6"}, {"tand", "0.00003"}, {"sigma", "1e-16[S/m]"}, {"k", "33[W/(m*K)]; 33[W/(m*K)]; 35[W/(m*K)]"},
                    {"rho", "3980[kg/m^3]"}, {"Cp", "761[J/(kg*K)]"}, {"mur", "1"}, {"alpha", "4.5e-6[1/K]; 4.5e-6[1/K]; 5.3e-6[1/K]"},
                    {"nrange", "0.2[um] 5[um]"}, {"orientation", "cut"}, {"cut", "z"}, {"along", "x"}}),
             {{"n", sellmeier(1, {{1.4313493, 0.0726631}, {0.65054713, 0.1193242}, {5.3414021, 18.028251}}) + QStringLiteral("; ") +
                        sellmeier(1, {{1.4313493, 0.0726631}, {0.65054713, 0.1193242}, {5.3414021, 18.028251}}) + QStringLiteral("; ") +
                        sellmeier(1, {{1.5039759, 0.0740288}, {0.55069141, 0.1216529}, {6.5927379, 20.072248}})},
              {"C", trigonalStiffness(497.6, 162.6, 117.2, -23.5, 501.8, 147.2)}}),
        with(entry("YIG", "Crystals",
                   "Yttrium iron garnet, Y₃Fe₅O₁₂ (cubic): n 2.21 near 1550 nm; magnetized, its gyration g -3.3e-4 (its Faraday rotation "
                   "about 175°/cm; θF = -π g/(λ n) about the magnetization, a wave feature's Magneto-optic option); its stiffness",
                   {{"epsilonr", "15"}, {"tand", "0.0002"}, {"sigma", "1e-12[S/m]"}, {"k", "6[W/(m*K)]"}, {"rho", "5170[kg/m^3]"},
                    {"Cp", "570[J/(kg*K)]"}, {"mur", "1"}, {"alpha", "10.4e-6[1/K]"}, {"n", "2.21"}, {"nrange", "1.2[um] 1.7[um]"},
                    {"g", "-3.3e-4"}}),
             {{"C", cubicStiffness(269, 107.7, 76.4)}}),
        with(entry("Ce:YIG", "Crystals",
                   "Cerium-substituted YIG, a magneto-optic garnet film: n 2.22 near 1550 nm; its gyration g 0.0086 (its Faraday rotation "
                   "about -4500°/cm, YIG's the other way) - an isolator's non-reciprocal phase shift; its stiffness YIG's",
                   {{"epsilonr", "15"}, {"tand", "0.0002"}, {"sigma", "1e-12[S/m]"}, {"k", "6[W/(m*K)]"}, {"rho", "5170[kg/m^3]"},
                    {"Cp", "570[J/(kg*K)]"}, {"mur", "1"}, {"alpha", "10.4e-6[1/K]"}, {"n", "2.22"}, {"nrange", "1.2[um] 1.7[um]"},
                    {"g", "0.0086"}}),
             {{"C", cubicStiffness(269, 107.7, 76.4)}}),
        // Optical glasses: n of the wavelength (Sellmeier's), for rays and modes.
        entry("Glass (BK7)", "Optical", "Schott N-BK7: n 1.5168 at 587.6 nm (Sellmeier's), for lenses and prisms",
              {{"epsilonr", "6.7"}, {"k", "1.114[W/(m*K)]"}, {"rho", "2510[kg/m^3]"}, {"Cp", "858[J/(kg*K)]"}, {"mur", "1"}, {"E", "82[GPa]"},
               {"nu", "0.206"}, {"alpha", "7.1e-6[1/K]"}, {"dndT", "1.6e-6[1/K]"},
               {"n", "sqrt(1 + 1.03961212*(lambda0/1[um])^2/((lambda0/1[um])^2 - 0.00600069867) + 0.231792344*(lambda0/1[um])^2/((lambda0/1[um])^2 - "
                     "0.0200179144) + 1.01046945*(lambda0/1[um])^2/((lambda0/1[um])^2 - 103.560653))"}}),
        with(entry("Fused silica", "Optical",
                   "SiO₂ glass: n 1.4585 at 587.6 nm (Malitson's Sellmeier, from 0.21 to 6.7 µm - Silicon dioxide's too); κ from "
                   "2.5 µm (Franta et al.'s); photoelastic p11 0.121, p12 0.270",
              {{"epsilonr", "3.8"}, {"k", "1.38[W/(m*K)]"}, {"rho", "2203[kg/m^3]"}, {"Cp", "740[J/(kg*K)]"}, {"mur", "1"}, {"E", "73[GPa]"},
               {"nu", "0.17"}, {"alpha", "0.55e-6[1/K]"}, {"dndT", "1.0e-5[1/K]"},
               {"n", "sqrt(1 + 0.6961663*(lambda0/1[um])^2/((lambda0/1[um])^2 - 0.0684043^2) + 0.4079426*(lambda0/1[um])^2/((lambda0/1[um])^2 - "
                     "0.1162414^2) + 0.8974794*(lambda0/1[um])^2/((lambda0/1[um])^2 - 9.896161^2))"}}),
             {{"p", cubicPhotoelastic(0.121, 0.270, (0.121 - 0.270) / 2)}, {"kappa", silicaExtinction()},
              {"nrange", QStringLiteral("0.21[um] 6.7[um]")}}),
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
        with(entry("Water", "Fluids",
                   "Pure water at 20 °C; n and κ from 0.2 to 200 µm (Daimon and Masumura's Sellmeier to 1 µm, Hale and Querry's "
                   "beyond: 1.318 at 1550 nm, κ 1e-4 there - its absorption bands at 1.45, 1.94 and 2.95 µm)",
                   {{"epsilonr", "80.1"}, {"sigma", "5.5e-6[S/m]"}, {"k", "0.598[W/(m*K)]"}, {"rho", "998[kg/m^3]"}, {"Cp", "4182[J/(kg*K)]"},
                    {"mur", "1"}, {"nrange", "0.2[um] 200[um]"}}),
             {{"n", waterIndex()}, {"kappa", waterExtinction()}}),
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
        if (propertyKeys().contains(it.key())) n.props.insert(it.key(), it.value());
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
    out << QStringLiteral("InGaAsP") << QStringLiteral("AlGaInAs") << QStringLiteral("AlGaInP");
    for (const Nitride& n : nitrides()) out << QString::fromLatin1(n.name);
    for (const NitrideAlloy& a : nitrideAlloys()) out << QString::fromLatin1(a.name);
    out << QStringLiteral("AlGaInN") << QStringLiteral("SiGe");
    return out;
}

bool isWurtzite(const QString& compound)
{
    const QString name = compound.trimmed();
    if (nitride(name) || name.compare(QLatin1String("AlGaInN"), Qt::CaseInsensitive) == 0) return true;
    for (const NitrideAlloy& a : nitrideAlloys())
        if (name.compare(QLatin1String(a.name), Qt::CaseInsensitive) == 0) return true;
    return false;
}

std::optional<WurtziteParameters> wurtziteParameters(const QString& compound, double x, double T, QString* error, double y)
{
    auto fail = [&](const QString& why) -> std::optional<WurtziteParameters> {
        if (error) *error = why;
        return std::nullopt;
    };
    const QString name = compound.trimmed();
    if (!(T > 0)) return fail(tr("a temperature of %1 K").arg(T));
    if (const Nitride* n = nitride(name)) return nitrideAt(*n, T);
    if (name.compare(QLatin1String("AlGaInN"), Qt::CaseInsensitive) == 0) {
        // Alx Gay In1-x-y N from its three ternaries (Vurgaftman and Meyer's,
        // as AlGaInAs's).
        const double z = 1 - x - y;
        if (!(x >= -1e-12 && y >= -1e-12 && z >= -1e-12))
            return fail(tr("AlGaInN: x = %1, y = %2 are not shares that add up to 1 or less").arg(x).arg(y));
        x = std::max(0.0, x);
        y = std::max(0.0, y);
        const double zz = std::max(0.0, z), d = x * y + y * zz + x * zz;
        if (d < 1e-12) return wurtziteParameters(QLatin1String(x > 0.5 ? "AlN" : y > 0.5 ? "GaN" : "InN"), 0, T, error);
        const double u = (1 + x - y) / 2, v = (1 + y - zz) / 2, w = (1 + x - zz) / 2;
        const std::optional<WurtziteParameters> alGa = wurtziteParameters(QStringLiteral("AlGaN"), u, T, error);
        const std::optional<WurtziteParameters> inGa = wurtziteParameters(QStringLiteral("InGaN"), 1 - v, T, error);   // (In's share 1 - v)
        const std::optional<WurtziteParameters> inAl = wurtziteParameters(QStringLiteral("InAlN"), 1 - w, T, error);   // (In's share 1 - w)
        if (!alGa || !inGa || !inAl) return std::nullopt;
        const double weights[3] = {x * y / d, y * zz / d, x * zz / d};
        const WurtziteParameters* parts[3] = {&*alGa, &*inGa, &*inAl};
        WurtziteParameters p;
        for (auto m : {&WurtziteParameters::Eg, &WurtziteParameters::Ev, &WurtziteParameters::dcr, &WurtziteParameters::dso,
                       &WurtziteParameters::mez, &WurtziteParameters::mep, &WurtziteParameters::a1, &WurtziteParameters::a2,
                       &WurtziteParameters::Ep, &WurtziteParameters::a, &WurtziteParameters::c, &WurtziteParameters::C11,
                       &WurtziteParameters::C12, &WurtziteParameters::C13, &WurtziteParameters::C33, &WurtziteParameters::Psp,
                       &WurtziteParameters::e31, &WurtziteParameters::e33, &WurtziteParameters::epsr, &WurtziteParameters::n}) {
            p.*m = 0;
            for (int k = 0; k < 3; ++k) p.*m += weights[k] * (parts[k]->*m);
        }
        for (int i = 0; i < 6; ++i) {
            p.A[i] = p.D[i] = 0;
            for (int k = 0; k < 3; ++k) {
                p.A[i] += weights[k] * parts[k]->A[i];
                p.D[i] += weights[k] * parts[k]->D[i];
            }
        }
        return p;
    }
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
        const std::optional<WurtziteParameters> w = wurtziteParameters(name, x, T, error, y);
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
    if (name.compare(QLatin1String("AlGaInP"), Qt::CaseInsensitive) == 0) {
        // Alx Gay In1-x-y P from AlGaP, GaInP, AlInP, as AlGaInAs.
        const double z = 1 - x - y;
        if (!inRange(x) || !inRange(y) || z < -1e-12)
            return fail(tr("AlGaInP: x = %1, y = %2 are not shares that add up to 1 or less").arg(x).arg(y));
        x = std::clamp(x, 0.0, 1.0);
        y = std::clamp(y, 0.0, 1.0);
        const double zz = std::max(0.0, z);
        const double d = x * y + y * zz + x * zz;
        if (d < 1e-12) {
            const char* corner = x > 0.5 ? "AlP" : y > 0.5 ? "GaP" : "InP";
            return binaryAt(*binary(QLatin1String(corner)), T, set);
        }
        const double u = (1 + x - y) / 2, v = (1 + y - zz) / 2, w = (1 + x - zz) / 2;
        const BandParameters alGa = ternaryAt(*ternary(QStringLiteral("AlGaP")), u, T, set);
        const BandParameters gaIn = ternaryAt(*ternary(QStringLiteral("GaInP")), v, T, set);   // (Ga's share v)
        const BandParameters alIn = ternaryAt(*ternary(QStringLiteral("AlInP")), w, T, set);   // (Al's share w)
        return blend(blend(alGa, x * y / d, gaIn, y * zz / d), 1, alIn, x * zz / d);
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

namespace {

/// An alloy's model parameter \a key from its model sets: those that have
/// it weighed by their shares (renormalized) - mobilities harmonically, the
/// rest and the temperature exponents linearly, as Lumerical's alloys
/// without bowings. Empty where none has it (Masetti's: silicon's alone).
QString alloyModel(const std::vector<std::pair<const ModelSet*, double>>& parts, const QString& key)
{
    if (parts.empty()) return {};
    const QString k = key;
    const int c = k.endsWith(QLatin1Char('n')) ? 0 : 1;
    // What a set has of the key: its value and exponent, or none.
    struct Value {
        bool has = false;
        double v = 0, eta = 0;
    };
    auto of = [&k, &c](const ModelSet& m) -> Value {
        if (k == QLatin1String("munmin") || k == QLatin1String("mupmin")) return {m.nRef[c] > 0, m.muMin[c], m.etaMuMin[c]};
        if (k == QLatin1String("nrefn") || k == QLatin1String("nrefp")) return {m.nRef[c] > 0, m.nRef[c], m.etaNRef[c]};
        if (k == QLatin1String("alphan") || k == QLatin1String("alphap")) return {m.nRef[c] > 0, m.alpha[c], m.etaAlpha[c]};
        if (k == QLatin1String("zetan") || k == QLatin1String("zetap")) return {m.zeta[c] != 0, m.zeta[c], 0};
        if (k == QLatin1String("vsatn") || k == QLatin1String("vsatp")) return {true, m.vsat[c], 0};
        if (k == QLatin1String("betan") || k == QLatin1String("betap")) return {true, m.beta[c], 0};
        if (k == QLatin1String("Ebgn")) return {m.bgn[0] > 0, m.bgn[0], 0};
        if (k == QLatin1String("Nbgn")) return {m.bgn[0] > 0, m.bgn[1], 0};
        if (k == QLatin1String("Cbgn")) return {m.bgn[0] > 0, m.bgn[2], 0};
        if (k == QLatin1String("N0n") || k == QLatin1String("N0p")) return {true, m.fossum[c], 0};
        if (k == QLatin1String("fA")) return {true, m.fossum[2], 0};
        if (k == QLatin1String("fB")) return {true, m.fossum[3], 0};
        if (k == QLatin1String("fC")) return {true, m.fossum[4], 0};
        if (k == QLatin1String("fE")) return {true, m.fossum[5], 0};
        if (k == QLatin1String("mtn") || k == QLatin1String("mtp")) return {true, m.mt[c], 0};
        if (k == QLatin1String("caun") || k == QLatin1String("caup")) return {true, m.auger[c], 0};
        if (k == QLatin1String("copt")) return {true, m.rad, m.radEta};
        if (k == QLatin1String("iian") || k == QLatin1String("iiap")) return {m.ii[3 * c] > 0, m.ii[3 * c], 0};
        if (k == QLatin1String("iibn") || k == QLatin1String("iibp")) return {m.ii[3 * c] > 0, m.ii[3 * c + 1], 0};
        if (k == QLatin1String("iibetan") || k == QLatin1String("iibetap")) return {m.ii[3 * c] > 0, m.ii[3 * c + 2], 0};
        if (k == QLatin1String("bbtA")) return {m.bbt[0] > 0, m.bbt[0], 0};
        if (k == QLatin1String("bbtB")) return {m.bbt[0] > 0, m.bbt[1], 0};
        if (k == QLatin1String("bbtgamma")) return {m.bbt[0] > 0, m.bbt[2], 0};
        return {};
    };
    double total = 0, value = 0, inverse = 0, eta = 0;
    const bool harmonic = k.startsWith(QLatin1String("mu"));
    for (const auto& [m, w] : parts) {
        const Value v = of(*m);
        if (!v.has || !(w > 0)) continue;
        total += w;
        value += w * v.v;
        if (harmonic) inverse += w / v.v;
        eta += w * v.eta;
    }
    if (!(total > 0)) return {};
    const double v = harmonic ? total / inverse : value / total;
    eta /= total;
    if (k.startsWith(QLatin1String("mu"))) return ofT(v, QStringLiteral("cm^2/(V*s)"), eta);
    if (k.startsWith(QLatin1String("nref")) || k == QLatin1String("Nbgn") || k.startsWith(QLatin1String("N0")))
        return ofT(v, QStringLiteral("1/cm^3"), eta);
    if (k.startsWith(QLatin1String("vsat"))) return ofT(v, QStringLiteral("cm/s"), 0);
    if (k == QLatin1String("Ebgn")) return ofT(v, QStringLiteral("eV"), 0);
    if (k.startsWith(QLatin1String("cau"))) return ofT(v, QStringLiteral("cm^6/s"), 0);
    if (k == QLatin1String("copt")) return ofT(v, QStringLiteral("cm^3/s"), eta);
    if (k == QLatin1String("iian") || k == QLatin1String("iiap")) return ofT(v, QStringLiteral("1/cm"), 0);
    if (k == QLatin1String("iibn") || k == QLatin1String("iibp") || k == QLatin1String("bbtB")) return ofT(v, QStringLiteral("V/cm"), 0);
    return ofT(v, QString(), eta);
}

/// Silicon germanium's property \a key at Ge's share \a x and temperature
/// \a T: its X valley's gap (Si's and Ge's X gaps, Lumerical's bowing of
/// -0.4 eV) and L valley's (Si's 2.01 eV, Ge's), the lower; the valence
/// band's top straight between Si's and Ge's (χ from it); the mobilities
/// harmonic with the alloy's scattering (1/140 and 1/100 V s/cm² of x(1 -
/// x), Lumerical's), the conductivity of heat with its (1/2.8 m K/W); the
/// rest straight between the two. Empty for what it has none of.
QString siliconGermanium(const QString& key, double x, double T)
{
    x = std::clamp(x, 0.0, 1.0);
    auto lin = [x](double si, double ge) { return (1 - x) * si + x * ge; };
    const double egSi = 1.12 - 4.73e-4 * (T * T / (T + 636) - 9e4 / 936), egGe = 0.66 - 4.774e-4 * (T * T / (T + 235) - 9e4 / 535);
    const double eX = lin(egSi, 0.93) - 0.4 * x * (1 - x), eL = lin(2.01, egGe);
    const double Eg = std::min(eX, eL);
    const double t = T / 300;
    const QString k = key;
    if (k == QLatin1String("Eg")) return QStringLiteral("%1[eV]").arg(number(Eg));
    if (k == QLatin1String("chi")) return QStringLiteral("%1[eV]").arg(number(lin(4.05 + egSi, 4.0 + egGe) - Eg));
    if (k == QLatin1String("epsilonr")) return number(lin(11.7, 16.2));
    if (k == QLatin1String("Nc")) return QStringLiteral("%1[1/cm^3]").arg(number(lin(2.8e19, 1.04e19) * std::pow(t, 1.5)));
    if (k == QLatin1String("Nv")) return QStringLiteral("%1[1/cm^3]").arg(number(lin(1.04e19, 6.0e18) * std::pow(t, 1.5)));
    if (k == QLatin1String("mun") || k == QLatin1String("mup")) {
        const bool n = k == QLatin1String("mun");
        const double si = (n ? 1400 : 450) * std::pow(t, n ? -2.5 : -2.2), ge = (n ? 3900 : 1900) * std::pow(t, n ? -1.66 : -2.33);
        return QStringLiteral("%1[cm^2/(V*s)]").arg(number(1 / ((1 - x) / si + x / ge + x * (1 - x) / (n ? 140.0 : 100.0))));
    }
    if (k == QLatin1String("taun") || k == QLatin1String("taup")) return QStringLiteral("10[us]");
    if (k == QLatin1String("k")) return QStringLiteral("%1[W/(m*K)]").arg(number(1 / ((1 - x) / 130 + x / 60 + x * (1 - x) / 2.8)));
    if (k == QLatin1String("rho")) return QStringLiteral("%1[kg/m^3]").arg(number(lin(2329, 5323)));
    if (k == QLatin1String("Cp")) return QStringLiteral("%1[J/(kg*K)]").arg(number(lin(700, 320)));
    if (k == QLatin1String("E")) return QStringLiteral("%1[GPa]").arg(number(lin(170, 103)));
    if (k == QLatin1String("nu")) return number(lin(0.28, 0.26));
    if (k == QLatin1String("alpha")) return QStringLiteral("%1[1/K]").arg(number(lin(2.6e-6, 5.9e-6)));
    if (k == QLatin1String("n")) return number(lin(3.476, 4.275));
    if (k == QLatin1String("nrange")) return QStringLiteral("1.5[um] 1.6[um]");
    if (k == QLatin1String("dndT")) return QStringLiteral("%1[1/K]").arg(number(lin(1.86e-4, 4.2e-4)));
    if (k == QLatin1String("mur")) return QStringLiteral("1");
    if (semiconductorModelKeys().contains(k)) return alloyModel(modelShares(QStringLiteral("SiGe"), x, 0), k);
    return {};
}

} // namespace

std::optional<std::pair<double, double>> compositionForGap(const QString& compound, double gap, const QString& lattice, BandSet set,
                                                           QString* error, double T)
{
    auto fail = [&](const QString& why) -> std::optional<std::pair<double, double>> {
        if (error) *error = why;
        return std::nullopt;
    };
    const QString name = compound.trimmed();
    const bool quaternary = name.compare(QLatin1String("InGaAsP"), Qt::CaseInsensitive) == 0 ||
                            name.compare(QLatin1String("AlGaInAs"), Qt::CaseInsensitive) == 0 ||
                            name.compare(QLatin1String("AlGaInP"), Qt::CaseInsensitive) == 0;
    if (!ternary(name) && !quaternary)
        return fail(tr("%1 has no composition to solve for (a ternary or InGaAsP, AlGaInAs, AlGaInP has)").arg(name));
    double a = 0;
    if (quaternary) {
        const std::optional<BandParameters> l = bandParameters(lattice, 0, 0, T, nullptr, set);
        if (!binary(lattice.trimmed()) || !l) return fail(tr("its lattice: %1 is not a binary (InP, GaAs...)").arg(lattice));
        a = l->a0;
    }
    // Along its line, u from 0 to 1: a ternary's x; a quaternary's
    // composition on the lattice - InGaAsP's y (As), x (Ga) matching it;
    // AlGaInAs's Al share u of Al and Ga, their sum matching it.
    auto along = [&name, quaternary, a, T, set](double u, double* x, double* y) {
        if (!quaternary) {
            *x = u;
            *y = 0;
            return true;
        }
        const bool ingaasp = name.compare(QLatin1String("InGaAsP"), Qt::CaseInsensitive) == 0;
        auto lat = [&](double s) {
            const std::optional<BandParameters> p = ingaasp ? bandParameters(name, s, u, T, nullptr, set) : bandParameters(name, u * s, (1 - u) * s, T, nullptr, set);
            return p ? p->a0 - a : NAN;
        };
        double lo = 0, hi = 1, flo = lat(lo), fhi = lat(hi);
        if (!(flo * fhi <= 0)) return false;
        // (An end on the lattice: that end - InP itself at y 0.)
        if (flo == 0) hi = lo;
        else if (fhi == 0) lo = hi;
        for (int k = 0; k < 100 && hi - lo > 1e-13; ++k) {
            const double mid = 0.5 * (lo + hi), f = lat(mid);
            if ((f <= 0) == (flo <= 0)) {
                lo = mid;
                flo = f;
            } else {
                hi = mid;
            }
        }
        const double s = 0.5 * (lo + hi);
        *x = ingaasp ? s : u * s;
        *y = ingaasp ? u : (1 - u) * s;
        return true;
    };
    auto gapAt = [&name, &along, gap, T, set](double u) {
        double x = 0, y = 0;
        if (!along(u, &x, &y)) return double(NAN);
        const std::optional<BandParameters> p = bandParameters(name, x, y, T, nullptr, set);
        return p ? p->Eg - gap : NAN;
    };
    const int n = 400;
    double least = INFINITY, most = -INFINITY, previous = gapAt(0), u0 = 0;
    for (int k = 1; k <= n; ++k) {
        const double u = double(k) / n, f = gapAt(u);
        if (std::isfinite(f)) {
            least = std::min(least, f + gap);
            most = std::max(most, f + gap);
        }
        if (std::isfinite(previous)) {
            least = std::min(least, previous + gap);
            most = std::max(most, previous + gap);
        }
        if (std::isfinite(previous) && std::isfinite(f) && (previous == 0 || previous * f < 0)) {
            double lo = u0, hi = u, flo = previous;
            for (int i = 0; i < 100 && hi - lo > 1e-13; ++i) {
                const double mid = 0.5 * (lo + hi), fm = gapAt(mid);
                if ((fm <= 0) == (flo <= 0)) {
                    lo = mid;
                    flo = fm;
                } else {
                    hi = mid;
                }
            }
            double x = 0, y = 0;
            along(previous == 0 ? u0 : 0.5 * (lo + hi), &x, &y);
            return std::pair{x, y};
        }
        previous = f;
        u0 = u;
    }
    if (!std::isfinite(least)) return fail(tr("%1 has no composition on %2's lattice").arg(name, lattice));
    return fail(tr("%1's gap of %2 eV is not reached%3: its gaps there from %4 to %5 eV")
                    .arg(name, formatNumber(gap, 4), quaternary ? tr(" on %1's lattice").arg(lattice) : QString(), formatNumber(least, 4),
                         formatNumber(most, 4)));
}

QString compoundProperty(const QString& compound, double x, double y, const QString& key, BandSet set, double T)
{
    if (!(T > 0)) T = 300;
    if (compound.trimmed().compare(QLatin1String("SiGe"), Qt::CaseInsensitive) == 0) return siliconGermanium(key, x, T);
    if (semiconductorModelKeys().contains(key)) return alloyModel(modelShares(compound.trimmed(), x, y), key);
    const std::optional<BandParameters> p = bandParameters(compound, x, y, T, nullptr, set);
    if (!p) return {};
    auto num = [](double v) { return formatNumber(v, 8); };
    auto eV = [&](double v) { return num(v) + QStringLiteral("[eV]"); };
    const QString k = key;
    const QString name = compound.trimmed();
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
    // The index and absorption: a nitride's index constant (near its gap);
    // a zinc blende's AlGaAs's Afromowitz's, InP's Pettit and Turner's,
    // InGaAsP's Adachi's (on InP's lattice), the others' Afromowitz's
    // oscillator of their binaries (binaryOptics), at the gap measured at
    // 300 K (of T), held above it, where its gap's absorption is.
    const bool algaas = name.compare(QLatin1String("AlGaAs"), Qt::CaseInsensitive) == 0, gaas = name.compare(QLatin1String("GaAs"), Qt::CaseInsensitive) == 0;
    const bool inp = name.compare(QLatin1String("InP"), Qt::CaseInsensitive) == 0;
    if (k == QLatin1String("n") || k == QLatin1String("kappa") || k == QLatin1String("nrange")) {
        std::vector<std::pair<const Binary*, double>> parts;
        if (!isWurtzite(name)) parts = shares(name, x, y);
        const std::optional<BandParameters> m = bandParameters(compound, x, y, T, nullptr, BandSet::Measured);
        if (parts.empty() || !m) return k == QLatin1String("n") ? num(p->n) : QString();
        double E0 = 0, Ed = 0, C = 0, to = 1e9;
        std::vector<std::pair<double, double>> lattice;
        for (const auto& [b, w] : parts) {
            const BinaryOptics* o = opticsOf(b->name);
            if (!o) return k == QLatin1String("n") ? num(p->n) : QString();
            E0 += w * o->E0;
            Ed += w * o->Ed;
            C += w * o->A / edgeScale(binaryAt(*b, 300));
            lattice.emplace_back(w * o->S, o->ETO);
            to = std::min(to, o->to);
        }
        const double Eg = m->Eg;
        if (k == QLatin1String("kappa")) return edgeExtinction(parts.size() == 1 ? opticsOf(parts.front().first->name)->A : C * edgeScale(*m), Eg);
        if (k == QLatin1String("n")) {
            if (algaas) return afromowitzIndex(std::clamp(x, 0.0, 1.0));
            if (name.compare(QLatin1String("InGaAsP"), Qt::CaseInsensitive) == 0) return adachiIndex(std::clamp(y, 0.0, 1.0), Eg, Eg + m->Delta);
            // (Pettit and Turner's from 0.95 µm - its pole at 0.626 µm -, the
            // oscillator below, the two weighed from 0.95 to 1 µm.)
            if (inp)
                return QStringLiteral("if(lambda0 < 0.95[um], %1, if(lambda0 > 1[um], %2, ((1[um] - lambda0)*%1 + (lambda0 - 0.95[um])*%2)/0.05[um]))")
                    .arg(oscillatorIndex(E0, Ed, Eg, lattice), sellmeier(7.255, {{2.316, 0.6263}, {2.765, 32.935}}));
            return oscillatorIndex(E0, Ed, Eg, lattice);
        }
        // (From 0.1 eV above the gap - GaP's from 0.8 µm, its oscillator
        // 0.04 off nearer its gap -, to the longest wavelength its binaries
        // were fitted to; AlGaAs's Afromowitz's to 5 µm.)
        double from = 1.23984198 / (Eg + 0.1);
        if (name.compare(QLatin1String("GaP"), Qt::CaseInsensitive) == 0) from = std::max(from, 0.8);
        if (algaas) to = 5;
        return QStringLiteral("%1[um] %2[um]").arg(number(std::round(from * 1000) / 1000), number(to));
    }
    if (k == QLatin1String("chi")) {
        // Its conduction band against GaAs's (χ 4.07 eV): the offsets of
        // the common scale.
        const BandParameters gaasAtT = binaryAt(*binary(QStringLiteral("GaAs")), T, set);
        return eV(4.07 - ((p->VBO + p->Eg) - (gaasAtT.VBO + gaasAtT.Eg)));
    }
    if (k == QLatin1String("Nc") || k == QLatin1String("Nv")) {
        // 2 (m kT / 2πħ²)^(3/2); the holes' mass of the heavy and light ones'.
        double m = p->me;
        if (k == QLatin1String("Nv")) {
            const double mhh = 1 / (p->gamma1 - 2 * p->gamma2), mlh = 1 / (p->gamma1 + 2 * p->gamma2);
            m = std::pow(std::pow(mhh, 1.5) + std::pow(mlh, 1.5), 2.0 / 3.0);
        }
        return num(2 * std::pow(m * M0 * KB * T / (2 * M_PI * Hbar * Hbar), 1.5)) + QStringLiteral("[1/m^3]");
    }
    if (k == QLatin1String("taun") || k == QLatin1String("taup")) return QStringLiteral("1[ns]");
    const auto parts = shares(name, x, y);
    if (parts.empty()) return {};
    double sum2 = 0;
    for (const auto& [b, w] : parts) sum2 += w * w;
    if (k == QLatin1String("mun") || k == QLatin1String("mup")) {
        // (Matthiessen's rule over the binaries, each (T/300 K)^-ζ: a rough
        // mobility.)
        const int c = k == QLatin1String("mun") ? 0 : 1;
        double inv = 0;
        for (const auto& [b, w] : parts) {
            const ModelSet* m = modelSet(QLatin1String(b->name));
            inv += w / ((c == 0 ? b->mun : b->mup) * std::pow(T / 300, m ? -m->zeta[c] : 0.0));
        }
        return num(1 / inv) + QStringLiteral("[cm^2/(V*s)]");
    }
    if (k == QLatin1String("k")) {
        // Thermal resistivities, with an alloy's disorder (0.3 m K/W of
        // bowing); of T as its binaries' exponents weighed.
        double w = 0.3 * (1 - sum2) / 2, eta = 0;
        for (const auto& [b, s] : parts) {
            w += s / b->k;
            eta += s * heatExponent(QLatin1String(b->name));
        }
        return ofT(1 / w, QStringLiteral("W/(m*K)"), eta);
    }
    if (k == QLatin1String("rho") || k == QLatin1String("Cp")) {
        double v = 0;
        for (const auto& [b, s] : parts) v += s * (k == QLatin1String("rho") ? b->rho : b->Cp);
        return num(v) + (k == QLatin1String("rho") ? QStringLiteral("[kg/m^3]") : QStringLiteral("[J/(kg*K)]"));
    }
    return {};
}

QStringList semiconductorModelKeys()
{
    static const QStringList keys = {
        QStringLiteral("munmin"), QStringLiteral("nrefn"), QStringLiteral("alphan"), QStringLiteral("mupmin"), QStringLiteral("nrefp"),
        QStringLiteral("alphap"), QStringLiteral("m1n"),   QStringLiteral("m2n"),    QStringLiteral("mu1n"),   QStringLiteral("pcn"),
        QStringLiteral("crn"),    QStringLiteral("csn"),   QStringLiteral("man"),    QStringLiteral("mbn"),    QStringLiteral("m1p"),
        QStringLiteral("m2p"),    QStringLiteral("mu1p"),  QStringLiteral("pcp"),    QStringLiteral("crp"),    QStringLiteral("csp"),
        QStringLiteral("map"),    QStringLiteral("mbp"),   QStringLiteral("zetan"),  QStringLiteral("zetap"),  QStringLiteral("vsatn"),
        QStringLiteral("vsatp"),  QStringLiteral("betan"), QStringLiteral("betap"),  QStringLiteral("Ebgn"),   QStringLiteral("Nbgn"),
        QStringLiteral("Cbgn"),   QStringLiteral("N0n"),   QStringLiteral("N0p"),    QStringLiteral("fA"),     QStringLiteral("fB"),
        QStringLiteral("fC"),     QStringLiteral("fE"),    QStringLiteral("mtn"),    QStringLiteral("mtp"),    QStringLiteral("Et"),
        QStringLiteral("caun"),   QStringLiteral("caup"),  QStringLiteral("copt"),   QStringLiteral("iian"),   QStringLiteral("iibn"),
        QStringLiteral("iian2"),  QStringLiteral("iibn2"), QStringLiteral("iiap"),   QStringLiteral("iibp"),   QStringLiteral("iiap2"),
        QStringLiteral("iibp2"),  QStringLiteral("iiE0"),  QStringLiteral("iibetan"), QStringLiteral("iibetap"), QStringLiteral("bbtA"),
        QStringLiteral("bbtB"),   QStringLiteral("bbtgamma"), QStringLiteral("cstim"), QStringLiteral("jthreshold"), QStringLiteral("tgain")};
    return keys;
}

QString materialKeyOf(const QString& type, const QString& key)
{
    if (type == QLatin1String("semiconductor/auger")) {
        if (key == QLatin1String("Cn")) return QStringLiteral("caun");
        if (key == QLatin1String("Cp")) return QStringLiteral("caup");
        return {};
    }
    if (type == QLatin1String("semiconductor/radiative")) return key == QLatin1String("B") ? QStringLiteral("copt") : QString();
    if (type == QLatin1String("semiconductor/impact")) {
        static const QHash<QString, QString> map = {
            {QStringLiteral("an"), QStringLiteral("iian")},   {QStringLiteral("bn"), QStringLiteral("iibn")},
            {QStringLiteral("ap"), QStringLiteral("iiap")},   {QStringLiteral("bp"), QStringLiteral("iibp")},
            {QStringLiteral("an2"), QStringLiteral("iian2")}, {QStringLiteral("bn2"), QStringLiteral("iibn2")},
            {QStringLiteral("ap2"), QStringLiteral("iiap2")}, {QStringLiteral("bp2"), QStringLiteral("iibp2")},
            {QStringLiteral("E0"), QStringLiteral("iiE0")},   {QStringLiteral("betan"), QStringLiteral("iibetan")},
            {QStringLiteral("betap"), QStringLiteral("iibetap")}};
        return map.value(key);
    }
    if (type == QLatin1String("semiconductor/btbt")) {
        if (key == QLatin1String("A")) return QStringLiteral("bbtA");
        if (key == QLatin1String("B")) return QStringLiteral("bbtB");
        if (key == QLatin1String("gamma")) return QStringLiteral("bbtgamma");
        return {};
    }
    if (type == QLatin1String("semiconductor/stimulated")) {
        if (key == QLatin1String("c")) return QStringLiteral("cstim");
        if (key == QLatin1String("Jth")) return QStringLiteral("jthreshold");
        if (key == QLatin1String("t")) return QStringLiteral("tgain");
        return {};
    }
    if (type == QLatin1String("semiconductor/model") || type == QLatin1String("semiconductor/srh")) {
        // (Not Lombardi's B, C, λ, δ - Cn, Cp the Auger's on a material -,
        // nor the dopants' levels.)
        static const QStringList feature = {QStringLiteral("Bn"),      QStringLiteral("Cn"),     QStringLiteral("lambdan"), QStringLiteral("deltan"),
                                            QStringLiteral("Bp"),      QStringLiteral("Cp"),     QStringLiteral("lambdap"), QStringLiteral("deltap"),
                                            QStringLiteral("lcrit")};
        if (feature.contains(key)) return {};
        if (semiconductorModelKeys().contains(key) || key == QLatin1String("taun") || key == QLatin1String("taup")) return key;
        return {};
    }
    return {};
}

MaterialScope::MaterialScope(const Scope& parent, double T) : Scope(&parent)
{
    setConstant(QStringLiteral("T"), T, Dim::temperature());
}

MaterialEntry entryOf(const Node& material)
{
    MaterialEntry e;
    e.name = material.name();
    e.category = tr("My materials");
    e.user = true;
    for (const QString& key : propertyKeys()) {
        const QString v = material.text(key);
        if (!v.trimmed().isEmpty()) e.properties.insert(key, v);
    }
    return e;
}

} // namespace qucs_s::fem
