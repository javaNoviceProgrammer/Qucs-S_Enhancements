/*
 * test_fem_semiconductor.cpp - the multiphysics solver's semiconductors
 *                              against what is known: a bar's resistance,
 *                              a junction's built-in voltage and depletion,
 *                              a diode's current, a MOS capacitor's C-V
 *                              curves, carriers' decay in time
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_materials.h"
#include "fem_results.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QtTest>

#include <cmath>
#include <vector>

using namespace qucs_s::fem;

namespace {

constexpr double Q = 1.602176634e-19, KB = 1.380649e-23, Eps0 = 8.8541878188e-12;

bool slowBuild()
{
    // (QUCS_TEST_SLOW: a slow build's path in any, to check it quickly.)
    if (qEnvironmentVariableIsSet("QUCS_TEST_SLOW")) return true;
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#endif
#endif
    return false;
}

Model modelOf(const QByteArray& json)
{
    QString why;
    std::optional<Model> m = Model::fromJson(json, &why);
    if (!m) qFatal("not a model: %s", qPrintable(why));
    if (!why.isEmpty()) qWarning("%s", qPrintable(why));
    return *m;
}

struct Run {
    std::shared_ptr<ParameterScope> parameters;
    std::shared_ptr<Solution> solution;
    StudyRun study;
    QString error;
};

Run run(const Model& model)
{
    Run r;
    r.parameters = evaluateParameters(model);
    const GeometryBuild g = buildGeometry(model, *r.parameters);
    if (!g.ok()) {
        r.error = QStringLiteral("geometry: ") + QStringList(g.errors.values()).join(QLatin1String("; "));
        return r;
    }
    r.study = runStudy(model, r.parameters, g.topology, nullptr, QString());
    if (!r.study.ok()) {
        r.error = r.study.error;
        return r;
    }
    r.solution = r.study.solutions.back();
    return r;
}

double global(const Solution& s, const QString& name)
{
    const std::optional<double> v = s.global(name);
    return v ? *v : NAN;
}

double at(const Solution& s, const QString& expression, QPointF p)
{
    const Locator locator(s.mesh());
    const std::optional<double> v = evaluateAt(s, locator, expression, p);
    return v ? *v : NAN;
}

/// A bar of silicon (µm), its two ends ohmic contacts 1 and 2.
QByteArray bar(const QString& length, const QString& doping, const QString& v2, const QString& extra = {}, const QString& mesh = QStringLiteral("0.1"))
{
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "name": "bar", "size": ["%1", "1"]}],
        "materials": [{"material": "Silicon", "selection": {"all": true}}],
        "physics": [{"type": "semiconductor", "tag": "semi", "features": [
          %2
          {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}},
          {"type": "ohmic", "name": "2", "V0": "%3", "selection": {"of": ["bar"], "side": "right"}} %4]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%5"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
        .arg(length, doping, v2, extra, mesh)
        .toUtf8();
}


/// Silicon's intrinsic density at 300 K, as the library's Nc, Nv and Eg give it.
double siliconNi()
{
    const double vt = KB * 300 / Q;
    return std::sqrt(2.8e25 * 1.04e25) * std::exp(-1.12 / (2 * vt));
}

/// A pn junction of silicon (µm): p from 0 to L, n from L to 2L, 1 µm high;
/// the anode (a) at the p end at \a va, the cathode (k) at the other.
QByteArray junction(const QString& L, const QString& na, const QString& nd, const QString& va, const QString& steps,
                    const QString& extra = {}, QString hmax = {})
{
    // (A sanitized build: a coarser mesh.)
    if (hmax.isEmpty()) hmax = slowBuild() ? QStringLiteral("0.1") : QStringLiteral("0.05");
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "Va", "expression": "%4"}],
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "name": "pside", "size": ["%1", "1"]},
                     {"type": "rectangle", "name": "nside", "position": ["%1", "0"], "size": ["%1", "1"]}],
        "materials": [{"material": "Silicon", "selection": {"all": true}}],
        "physics": [{"type": "semiconductor", "tag": "semi", "features": [
          {"type": "doping", "dopant": "acceptor", "N0": "%2", "selection": {"only": ["pside"]}},
          {"type": "doping", "dopant": "donor", "N0": "%3", "selection": {"only": ["nside"]}},
          {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["pside"], "side": "left"}},
          {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["nside"], "side": "right"}} %6]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%7"}}],
      "studies": [{"type": "study", "steps": [%5]}]})json")
        .arg(L, na, nd, va, steps, extra, hmax)
        .toUtf8();
}

} // namespace

class TestFemSemiconductor : public QObject
{
    Q_OBJECT

private slots:
    void aResistor()
    {
        // n-type, 1e16 cm⁻³, 10 µm by 1 µm (1 m thick), 0.1 V across: I = q n µn V A / L.
        const Run r = run(modelOf(bar(QStringLiteral("10"),
                                      QStringLiteral(R"({"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"all": true}},)"),
                                      QStringLiteral("0.1[V]"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.solution;
        const double I2 = global(s, QStringLiteral("semi.I_2")), I1 = global(s, QStringLiteral("semi.I_1"));
        const double expected = Q * 1e22 * 0.14 * 0.1 * 1e-6 / 1e-5;
        qInfo("resistor: I %.6g A into 2, %.6g into 1 (q n µ V A/L %.6g); %s", I2, I1, expected, qPrintable(s.log.join(QLatin1String("; "))));
        QVERIFY2(std::abs(I2 / expected - 1) < 2e-3, qPrintable(QString::number(I2 / expected)));
        QVERIFY(std::abs(I1 + I2) < 1e-6 * std::abs(I2));
        // Its electrons: the doping; its potential's step: V/L.
        QVERIFY(std::abs(at(s, QStringLiteral("semi.n"), {5, 0.5}) / 1e22 - 1) < 1e-3);
        QVERIFY(std::abs(at(s, QStringLiteral("semi.Ex"), {5, 0.5}) / -1e4 - 1) < 2e-3);
        QVERIFY(std::abs(at(s, QStringLiteral("semi.Jnx"), {5, 0.5}) / (-expected / 1e-6) - 1) < 5e-3);
    }

    void aJunctionsDepletion()
    {
        // 1e16 cm⁻³ each side: its built-in voltage, and its small-signal
        // capacitance against the depletion approximation's ε/W, W from
        // Vbi - V - 2Vt, at 0 and -2 V.
        const double ni = siliconNi(), vt = KB * 300 / Q, N = 1e22, es = 11.7 * Eps0;
        const double vbi = vt * std::log(N * N / (ni * ni));
        // (A sanitized build: -2 V alone.)
        for (double va : slowBuild() ? std::vector<double>{-2.0} : std::vector<double>{0.0, -2.0}) {
            const Run r = run(modelOf(junction(QStringLiteral("5"), QStringLiteral("1e16[1/cm^3]"), QStringLiteral("1e16[1/cm^3]"),
                                               QStringLiteral("%1[V]").arg(va),
                                               QStringLiteral(R"({"type": "stationary"}, {"type": "frequency", "frequencies": "1[kHz]"})"))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.solution;
            const double C = global(s, QStringLiteral("semi.Ckk"));
            const double W = std::sqrt(2 * es * (2 * N) * (vbi - va - 2 * vt) / (Q * N * N));
            const double expected = es / W * 1e-6;
            const double built = at(s, QStringLiteral("semi.V"), {9.99, 0.5}) - at(s, QStringLiteral("semi.V"), {0.01, 0.5});
            const double emax = at(s, QStringLiteral("semi.normE"), {5, 0.5});
            qInfo("junction at %g V: C %.5g F (ε/W %.5g), the potential's step %.4f V (Vbi - Va %.4f), Emax %.4g V/m (%.4g); %s", va, C, expected,
                  built, vbi - va, emax, Q * N * W / 2 / es, qPrintable(s.log.filter(QStringLiteral("steps")).join(QString())));
            QVERIFY2(std::abs(C / expected - 1) < 0.03, qPrintable(QString::number(C / expected)));
            QVERIFY(std::abs(built - (vbi - va)) < 2e-3);
            QVERIFY(std::abs(emax / (Q * N * W / 2 / es) - 1) < 0.12);   // (the field of an element at the peak, and the depletion approximation's own)
            QVERIFY(std::abs(global(s, QStringLiteral("semi.Cak")) + C) < 1e-3 * C);
        }
    }

    // Silicon beside germanium (the library's: χ 4.05 and 4.0 eV, Eg 1.12
    // and 0.66). At equilibrium, n on both sides: each side's Ec - EF its
    // own kT ln(Nc/Nd), the Fermi level flat, the bands' steps Anderson's -
    // Ec by χSi - χGe, Ev by that and the gaps' difference. A p-Ge n⁺-Si
    // diode, short: its current the electrons injected into the germanium,
    // q Dn ni_Ge² / (Na W) (e^(V/Vt) - 1), forward and reversed.
    // An Al0.3Ga0.7N barrier 25 nm on GaN (Ga face, c up): the interface's
    // bound charge -ΔP its Polarization Charge's - the barrier stretched to
    // GaN's lattice, its piezoelectric part with its spontaneous one, σ =
    // P_GaN - P_AlGaN 0.024 C/m² (Ambacher et al.'s 1.5-1.7e13 q/cm²) -,
    // the same solution as a Surface
    // Charge of that σ, and a 2DEG under it in the GaN (above 1e18 cm⁻³);
    // the nitrogen face's the opposite charge: no electrons gathered.
    void wurtzitePolarizationCharge()
    {
        const std::optional<WurtziteParameters> g = wurtziteParameters(QStringLiteral("GaN"), 0, 300), b = wurtziteParameters(QStringLiteral("AlGaN"), 0.3, 300);
        QVERIFY(g && b);
        const double exx = (g->a - b->a) / b->a, ezz = -2 * b->C13 / b->C33 * exx;
        const double sigma = g->Psp - (b->Psp + 2 * b->e31 * exx + b->e33 * ezz);
        auto hemt = [&](const QString& interface, const QString& barrier = QStringLiteral("AlGaN")) {
            return modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "nm",
                "geometry": [{"type": "rectangle", "name": "gan", "size": ["50", "200"]},
                             {"type": "rectangle", "name": "barrier", "position": ["0", "200"], "size": ["50", "25"]}],
                "materials": [{"material": "GaN", "compound": "GaN", "mun": "1000[cm^2/(V*s)]", "mup": "30[cm^2/(V*s)]", "selection": {"only": ["gan"]}},
                              {"material": "AlGaN", "compound": "%3", "x": "0.3", "mun": "300[cm^2/(V*s)]", "mup": "10[cm^2/(V*s)]",
                               "selection": {"only": ["barrier"]}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"all": true}},
                  %1,
                  {"type": "ohmic", "name": "s", "V0": "0[V]", "selection": {"of": ["gan"], "side": "bottom"}},
                  {"type": "ohmic", "name": "t", "V0": "0[V]", "selection": {"of": ["barrier"], "side": "top"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "%2"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                               .arg(interface, slowBuild() ? QStringLiteral("2") : QStringLiteral("1"), barrier)
                               .toUtf8());
        };
        const QString at = QStringLiteral(R"("selection": {"of": ["barrier"], "side": "bottom"})");
        const Run pol = run(hemt(QStringLiteral(R"({"type": "polarization", "substrate": "GaN", %1})").arg(at)));
        const Run fixed = run(hemt(QStringLiteral(R"({"type": "charge", "rhos": "%1[C/m^2]", %2})").arg(sigma, 0, 'g', 17).arg(at)));
        const Run nface = run(hemt(QStringLiteral(R"({"type": "polarization", "substrate": "GaN", "polarity": "nitrogen", %1})").arg(at)));
        QVERIFY2(pol.error.isEmpty() && fixed.error.isEmpty() && nface.error.isEmpty(), qPrintable(pol.error + fixed.error + nface.error));
        const double nPol = ::at(*pol.solution, QStringLiteral("semi.n"), {25, 199}), nFixed = ::at(*fixed.solution, QStringLiteral("semi.n"), {25, 199});
        const double vPol = ::at(*pol.solution, QStringLiteral("semi.V"), {25, 200}), vFixed = ::at(*fixed.solution, QStringLiteral("semi.V"), {25, 200});
        const double nN = ::at(*nface.solution, QStringLiteral("semi.n"), {25, 199});
        qInfo("σ %.4f C/m² (%s); under it n %.3g cm⁻³ (a surface charge's %.3g), the nitrogen face's %.3g", sigma,
              qPrintable(pol.solution->log.filter(QStringLiteral("polarization charge")).join(QLatin1Char(' '))), nPol * 1e-6, nFixed * 1e-6, nN * 1e-6);
        QVERIFY2(sigma > 0.024 && sigma < 0.0272, qPrintable(QString::number(sigma)));
        QVERIFY2(std::abs(nPol / nFixed - 1) < 1e-9 && std::abs(vPol - vFixed) < 1e-9, qPrintable(QStringLiteral("%1 %2").arg(nPol / nFixed).arg(vPol - vFixed)));
        QVERIFY2(nPol > 1e24 && nN < 1e21, qPrintable(QStringLiteral("%1 %2").arg(nPol).arg(nN)));
        // Not between two nitrides: refused.
        const Run wrong = run(hemt(QStringLiteral(R"({"type": "polarization", %1})").arg(at), QStringLiteral("AlGaAs")));
        QVERIFY2(wrong.error.contains(QLatin1String("is not between two nitrides")), qPrintable(wrong.error));
    }

    void aHeterojunction()
    {
        const double vt = KB * 300 / Q;
        const QString hmax = slowBuild() ? QStringLiteral("0.1") : QStringLiteral("0.05");
        auto twoMaterials = [&](const QString& leftMaterial, const QString& rightMaterial, const QString& leftDoping, const QString& rightDoping,
                        const QString& leftWidth, const QString& rightWidth, const QString& va) {
            return modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "parameters": [{"name": "Va", "expression": "%7"}],
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "left", "size": ["%5", "1"]},
                             {"type": "rectangle", "name": "right", "position": ["%5", "0"], "size": ["%6", "1"]}],
                "materials": [{"material": "%1", "selection": {"only": ["left"]}}, {"material": "%2", "selection": {"only": ["right"]}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  %3, %4,
                  {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["left"], "side": "left"}},
                  {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["right"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "%8"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                               .arg(leftMaterial, rightMaterial, leftDoping, rightDoping, leftWidth, rightWidth, va, hmax)
                               .toUtf8());
        };
        auto doping = [](const char* dopant, const char* N, const char* domain) {
            return QStringLiteral(R"({"type": "doping", "dopant": "%1", "N0": "%2", "selection": {"only": ["%3"]}})")
                .arg(QString::fromLatin1(dopant), QString::fromLatin1(N), QString::fromLatin1(domain));
        };
        {
            const Run r = run(twoMaterials(QStringLiteral("Silicon"), QStringLiteral("Germanium"), doping("donor", "1e17[1/cm^3]", "left"),
                                   doping("donor", "1e17[1/cm^3]", "right"), QStringLiteral("2"), QStringLiteral("2"), QStringLiteral("0[V]")));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.solution;
            auto eV = [&](const char* what, double x) { return at(s, QStringLiteral("semi.") + QString::fromLatin1(what), {x, 0.5}) / Q; };
            const double ecSi = eV("Ec", 0.3), ecGe = eV("Ec", 3.7);
            const double dEc = eV("Ec", 2.0001) - eV("Ec", 1.9999), dEv = eV("Ev", 2.0001) - eV("Ev", 1.9999);
            double fermi = 0;
            for (double x : {0.3, 1.9, 1.9999, 2.0001, 2.1, 3.7}) fermi = std::max({fermi, std::abs(eV("Efn", x)), std::abs(eV("Efp", x))});
            qInfo("n-Si n-Ge at equilibrium: Ec - EF %.4f eV in silicon (kT ln(Nc/Nd) %.4f), %.4f in germanium (%.4f); its steps: Ec %.4f eV "
                  "(χSi - χGe 0.05), Ev %.4f (0.51); the Fermi level within %.2g eV of 0; I %g A; %s",
                  ecSi, vt * std::log(2.8e19 / 1e17), ecGe, vt * std::log(1.04e19 / 1e17), dEc, dEv, fermi, global(s, QStringLiteral("semi.I_a")),
                  qPrintable(s.log.filter(QStringLiteral("hetero")).join(QString())));
            QVERIFY(std::abs(ecSi - vt * std::log(2.8e19 / 1e17)) < 1e-3);
            QVERIFY(std::abs(ecGe - vt * std::log(1.04e19 / 1e17)) < 1e-3);
            QVERIFY(std::abs(dEc - 0.05) < 1e-3);
            QVERIFY(std::abs(dEv - 0.51) < 1e-3);
            QVERIFY(fermi < 1e-9);
            QCOMPARE(global(s, QStringLiteral("semi.I_a")), 0.0);
        }
        // The diode: p-Ge 1e17 over 4 µm, n⁺-Si 1e18 over 1 µm, no
        // recombination - the electrons diffuse across the germanium to its
        // contact (the holes' injection into the silicon ~(ni_Si/ni_Ge)² of it).
        const double niGe = std::sqrt(1.04e25 * 6.0e24) * std::exp(-0.66 / (2 * vt));
        const double Na = 1e23, Nd = 1e24, eGe = 16.2 * Eps0, eSi = 11.7 * Eps0;
        // The built-in voltage: the vacuum levels' step, (χ + Eg - kT ln(Nv/Na))_Ge - (χ + kT ln(Nc/Nd))_Si.
        const double vbi = (4.0 + 0.66 - vt * std::log(6.0e24 / Na)) - (4.05 + vt * std::log(2.8e25 / Nd));
        for (double va : {0.2, -0.5}) {
            const Run r = run(twoMaterials(QStringLiteral("Germanium"), QStringLiteral("Silicon"), doping("acceptor", "1e17[1/cm^3]", "left"),
                                   doping("donor", "1e18[1/cm^3]", "right"), QStringLiteral("4"), QStringLiteral("1"), QStringLiteral("%1[V]").arg(va)));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.solution;
            const double Ia = global(s, QStringLiteral("semi.I_a")), Ik = global(s, QStringLiteral("semi.I_k"));
            // The depletion in the germanium; the rest of it W.
            const double xp = std::sqrt(2 * eGe * eSi * (vbi - va - 2 * vt) * Nd / (Q * Na * (eGe * Na + eSi * Nd)));
            const double W = 4e-6 - xp;
            const double expected = Q * 3900e-4 * vt * niGe * niGe / (Na * W) * std::expm1(va / vt) * 1e-6;
            qInfo("p-Ge n-Si at %g V: I_a %.5g A (q Dn ni² / (Na W) (e^(V/Vt) - 1) %.5g, W %.3f µm), I_k %.5g; Vbi %.4f V", va, Ia, expected, W * 1e6,
                  Ik, vbi);
            QVERIFY2(std::abs(Ia / expected - 1) < 0.03, qPrintable(QString::number(Ia / expected)));
            QVERIFY(std::abs(Ia + Ik) < 1e-6 * std::abs(Ia));
        }
    }

    void contactCurrents()
    {
        // P+ P N N+ (1e19 and 1e17 cm⁻³; contacts on the 1e19), reversed
        // from 0 V. At 0 V, its first point, from equilibrium: no current at
        // all. Reversed: the depletion's generation, q ∫ R, to 1 % - a
        // contact's rows give it only to the last digits of a 1e19 contact's
        // fluxes (pA, the generation a few pA here). The two currents' sum
        // nought; two terminals' small-signal G_aa = G_kk = -G_ak. (Fine
        // across the junction, q ∫ R's triangles and the boxes' R agreeing.)
        const QString hmax = QStringLiteral("0.01");
        const Run r = run(modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "parameters": [{"name": "Va", "expression": "0[V]"}],
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "name": "pp", "size": ["0.25", "0.1"]},
                         {"type": "rectangle", "name": "p", "position": ["0.25", "0"], "size": ["0.5", "0.1"]},
                         {"type": "rectangle", "name": "n", "position": ["0.75", "0"], "size": ["0.5", "0.1"]},
                         {"type": "rectangle", "name": "nn", "position": ["1.25", "0"], "size": ["0.25", "0.1"]}],
            "materials": [{"material": "Silicon", "selection": {"all": true}}],
            "physics": [{"type": "semiconductor", "tag": "semi", "features": [
              {"type": "doping", "dopant": "acceptor", "N0": "1e19[1/cm^3]", "selection": {"only": ["pp"]}},
              {"type": "doping", "dopant": "acceptor", "N0": "1e17[1/cm^3]", "selection": {"only": ["p"]}},
              {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"only": ["n"]}},
              {"type": "doping", "dopant": "donor", "N0": "1e19[1/cm^3]", "selection": {"only": ["nn"]}},
              {"type": "srh", "selection": {"all": true}},
              {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["pp"], "side": "left"}},
              {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["nn"], "side": "right"}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.1",
                     "features": [{"type": "size", "level": "domain", "hmax": "%1", "selection": {"only": ["p", "n"]}}]}}],
          "studies": [{"type": "study", "steps": [{"type": "sweep", "parameters": [["Va", "0, -0.45, -0.5, -0.55", "V"]]},
                                                  {"type": "stationary"}, {"type": "frequency", "frequencies": "1[kHz], 1[MHz]"}]}]})json")
                                      .arg(hmax)
                                      .toUtf8()));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(int(r.study.solutions.size()), 4);
        Node generation;
        generation.type = QStringLiteral("integral");
        generation.props.insert(QStringLiteral("expression"), QStringLiteral("1.602176634e-19[C]*semi.R*1[m]"));
        generation.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}});
        QStringList each;
        double before = 0;
        for (std::size_t i = 0; i < r.study.solutions.size(); ++i) {
            Solution& s = *r.study.solutions[i];
            s.select(-1);
            const double Ia = global(s, QStringLiteral("semi.I_a")), Ik = global(s, QStringLiteral("semi.I_k"));
            const double Gaa = global(s, QStringLiteral("semi.Gaa")), Gkk = global(s, QStringLiteral("semi.Gkk"));
            const double Gak = global(s, QStringLiteral("semi.Gak")), Gka = global(s, QStringLiteral("semi.Gka"));
            const DerivedResult d = evaluateDerived(s, generation);
            QVERIFY2(d.error.isEmpty() && d.values.size() == 1, qPrintable(d.error));
            const double qR = d.values[0].value;
            each << QStringLiteral("%1: I_a %2, I_k %3 (q∫R %4); G %5 %6 %7 %8")
                        .arg(s.label)
                        .arg(Ia, 0, 'g', 5)
                        .arg(Ik, 0, 'g', 5)
                        .arg(qR, 0, 'g', 5)
                        .arg(Gaa, 0, 'g', 7)
                        .arg(Gkk, 0, 'g', 7)
                        .arg(Gak, 0, 'g', 7)
                        .arg(Gka, 0, 'g', 7);
            const QByteArray said = each.join(QLatin1String("; ")).toUtf8();
            if (i == 0) {
                QVERIFY2(Ia == 0 && Ik == 0, said.constData());
            } else {
                QVERIFY2(std::abs(Ia + Ik) < 1e-6 * std::abs(Ia), said.constData());
                QVERIFY2(std::abs(Ia / qR - 1) < 0.01, said.constData());
                QVERIFY2(-Ia > before, said.constData());
                before = -Ia;
            }
            QVERIFY2(Gaa > 0 && std::abs(Gkk / Gaa - 1) < 1e-4 && std::abs(Gak / Gaa + 1) < 1e-4 && std::abs(Gka / Gaa + 1) < 1e-4, said.constData());
            // Its capacitance the depletion's, ε/W (0.1 µm by 1 m), W from Vbi - V - 2Vt.
            const double va = std::vector<double>{0, -0.45, -0.5, -0.55}[i], vt = KB * 300 / Q, N = 1e23, es = 11.7 * Eps0, ni = siliconNi();
            const double W = std::sqrt(2 * es * (vt * std::log(N * N / (ni * ni)) - va - 2 * vt) * 2 / (Q * N));
            const double Caa = global(s, QStringLiteral("semi.Caa"));
            QVERIFY2(std::abs(Caa / (es / W * 0.1e-6) - 1) < 0.03 && std::abs(global(s, QStringLiteral("semi.Cak")) / Caa + 1) < 1e-4,
                     qPrintable(QStringLiteral("%1: C %2, ε/W %3").arg(s.label).arg(Caa).arg(es / W * 0.1e-6)));
        }
        // At 1 kHz, G the dc current's slope (at -0.5 V, from -0.45 and -0.55 V).
        Solution& middle = *r.study.solutions[2];
        middle.select(0);
        const double Glow = global(middle, QStringLiteral("semi.Gaa"));
        r.study.solutions[1]->select(-1);
        r.study.solutions[3]->select(-1);
        const double slope = (global(*r.study.solutions[3], QStringLiteral("semi.I_a")) - global(*r.study.solutions[1], QStringLiteral("semi.I_a"))) / -0.1;
        each << QStringLiteral("at 1 kHz and -0.5 V: G %1 (dI/dV %2)").arg(Glow, 0, 'g', 5).arg(slope, 0, 'g', 5);
        QVERIFY2(std::abs(Glow / slope - 1) < 0.01, qPrintable(each.last()));
        qInfo("contact currents: %s", qPrintable(each.join(QLatin1String("; "))));
        // An ohmic contact's charge: said to be about nought, its capacitance
        // the small-signal's.
        bool described = false;
        for (const Variable& v : r.study.solutions.back()->globals())
            if (v.name == QLatin1String("semi.Q_a")) described = v.description.contains(QLatin1String("about nought")) && v.description.contains(QLatin1String("semi.Caa"));
        QVERIFY(described);
    }

    void currentsInTime()
    {
        // A junction (1e16 cm⁻³ each side) reversed by a ramp, -2 V in 1 µs:
        // the current at -1 V its capacitance there times the ramp (the
        // carriers' change in time weighs in the current where the contact's
        // weight is), the cathode's the same out.
        const QString hmax = slowBuild() ? QStringLiteral("0.25") : QStringLiteral("0.1");
        Model m = modelOf(junction(QStringLiteral("5"), QStringLiteral("1e16[1/cm^3]"), QStringLiteral("1e16[1/cm^3]"), QStringLiteral("0[V]"),
                                   QStringLiteral(R"x({"type": "stationary"}, {"type": "transient", "times": "range(0, 0.25[us], 0.5[us])", "rtol": "1e-4"})x"),
                                   QString(), hmax));
        for (Node& f : m.find(QStringLiteral("semi"))->children)
            if (f.text(QStringLiteral("name")) == QLatin1String("a")) f.props.insert(QStringLiteral("V0"), QStringLiteral("-2[V]*t/1[us]"));
        const Run r = run(m);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        QCOMPARE(s.snapshotCount(), 3);
        s.select(2);
        const double Ia = global(s, QStringLiteral("semi.I_a")), Ik = global(s, QStringLiteral("semi.I_k"));
        const Run c = run(modelOf(junction(QStringLiteral("5"), QStringLiteral("1e16[1/cm^3]"), QStringLiteral("1e16[1/cm^3]"), QStringLiteral("-1[V]"),
                                           QStringLiteral(R"({"type": "stationary"}, {"type": "frequency", "frequencies": "1[kHz]"})"), QString(), hmax)));
        QVERIFY2(c.error.isEmpty(), qPrintable(c.error));
        const double C = global(*c.solution, QStringLiteral("semi.Caa"));
        const double expected = C * -2e6;
        qInfo("in time: at -1 V I_a %.6g A, I_k %.6g (C dV/dt %.6g, C %.5g F); %s", Ia, Ik, expected, C, qPrintable(s.log.join(QLatin1String("; "))));
        QVERIFY2(std::abs(Ia / expected - 1) < 0.01, qPrintable(QString::number(Ia / expected)));
        QVERIFY2(std::abs(Ia + Ik) < 1e-3 * std::abs(Ia), qPrintable(QString::number(Ik / Ia)));
    }

    void aDiodesReverseRecovery()
    {
        // A long diode (1e16 cm⁻³ each side, 5 µm, lifetimes 0.1 and 0.3 ns:
        // diffusion lengths below a micrometre) forward at 0.65 V, switched
        // to -1 V in a picosecond: the minority carriers stored come out of
        // the anode - a reverse current far above the steady one, for some
        // tenths of a nanosecond. Between the two lifetimes the charge out
        // differs as the charge stored at rest does (q∫ of the minority
        // carriers beyond equilibrium's, each side): the depletion's charge,
        // the same for both, cancels; what recombines while they come out is
        // little: most come out in some tens of picoseconds, those far from
        // the junction in a lifetime or so - 1.2 ns, four of the longer (a
        // sanitized build: 0.6 ns, two; some of the tail not yet out).
        // (A sanitized build: coarser in space and time.)
        const bool slow = slowBuild();
        const QString hmax = slow ? QStringLiteral("0.25") : QStringLiteral("0.1");
        struct Out {
            double stored = 0, moved = 0, forward = 0, peak = 0;
        };
        auto recover = [&](const QString& tau, Out* out) {
            const QString srh = QStringLiteral(R"(, {"type": "srh", "source": "user", "taun": "%1", "taup": "%1", "selection": {"all": true}})").arg(tau);
            const QString steps = slow ? QStringLiteral(R"x({"type": "stationary"}, {"type": "transient", "times": "range(0, 1[ps], 5[ps]), range(7[ps], 2[ps], 25[ps]), range(30[ps], 10[ps], 100[ps]), range(150[ps], 50[ps], 0.6[ns])", "rtol": "3e-3"})x")
                                       : QStringLiteral(R"x({"type": "stationary"}, {"type": "transient", "times": "range(0, 0.5[ps], 5[ps]), range(6[ps], 1[ps], 30[ps]), range(35[ps], 5[ps], 100[ps]), range(120[ps], 20[ps], 300[ps]), range(350[ps], 50[ps], 1.2[ns])", "rtol": "1e-4"})x");
            Model m = modelOf(junction(QStringLiteral("5"), QStringLiteral("1e16[1/cm^3]"), QStringLiteral("1e16[1/cm^3]"), QStringLiteral("0.65[V]"), steps, srh, hmax));
            for (Node& f : m.find(QStringLiteral("semi"))->children)
                if (f.text(QStringLiteral("name")) == QLatin1String("a")) f.props.insert(QStringLiteral("V0"), QStringLiteral("0.65[V] - 1.65[V]*min(t/1[ps], 1)"));
            const Run r = run(m);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            Solution& s = *r.solution;
            QVERIFY(s.snapshotCount() > 30);
            s.select(0);
            out->forward = global(s, QStringLiteral("semi.I_a"));
            // Stored: q∫ (n - ni²/Na) over the p side, (p - ni²/Nd) over the n side (1 m thick).
            for (const auto& [side, expression] : {std::pair{"pside", "1.602176634e-19[C]*(semi.n - semi.ni^2/1e22[1/m^3])*1[m]"},
                                                   std::pair{"nside", "1.602176634e-19[C]*(semi.p - semi.ni^2/1e22[1/m^3])*1[m]"}}) {
                Node integral;
                integral.type = QStringLiteral("integral");
                integral.props.insert(QStringLiteral("expression"), QString::fromLatin1(expression));
                integral.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QString::fromLatin1(side)}}});
                const DerivedResult d = evaluateDerived(s, integral);
                QVERIFY2(d.error.isEmpty() && d.values.size() == 1, qPrintable(d.error));
                out->stored += d.values[0].value;
            }
            // Out of the anode beyond the reverse current at the end.
            const int last = s.snapshotCount() - 1;
            s.select(last);
            const double reverse = global(s, QStringLiteral("semi.I_a"));
            double before = 0, tb = 0;
            for (int k = 0; k <= last; ++k) {
                s.select(k);
                const double t = s.snapshotTime(k), I = global(s, QStringLiteral("semi.I_a")) - reverse;
                if (k > 0) out->moved -= 0.5 * (I + before) * (t - tb);
                out->peak = std::min(out->peak, I + reverse);
                before = I;
                tb = t;
            }
            qInfo("τ %s: forward %.5g A, stored %.5g C (I_F τ %.5g); out of the anode %.5g C, its reverse current at most %.5g A (steady %.4g)",
                  qPrintable(tau), out->forward, out->stored, out->forward * (tau.startsWith(QLatin1String("0.1")) ? 1e-10 : 3e-10), out->moved, out->peak,
                  reverse);
        };
        Out a, b;
        recover(QStringLiteral("0.1[ns]"), &a);
        if (QTest::currentTestFailed()) return;
        recover(QStringLiteral("0.3[ns]"), &b);
        if (QTest::currentTestFailed()) return;
        const double k = (b.moved - a.moved) / (b.stored - a.stored);
        qInfo("the charge out, between the lifetimes: %.4f of the charge stored's difference", k);
        QVERIFY2(k > (slow ? 0.8 : 0.85) && k < (slow ? 1.05 : 1.02), qPrintable(QString::number(k)));
        QVERIFY(a.peak < -10 * a.forward && b.peak < -10 * b.forward);
    }

    void aShortDiodesCurrent()
    {
        // Lifetimes long: no recombination; forward 0.4 V - Shockley's
        // short-base diode, J0 = q ni² (Dn/(Na Wp) + Dp/(Nd Wn)), the neutral
        // widths less the depletion's.
        const double ni = siliconNi(), vt = KB * 300 / Q, N = 1e22, es = 11.7 * Eps0;
        const double vbi = vt * std::log(N * N / (ni * ni));
        const QString srh = QStringLiteral(R"(, {"type": "srh", "source": "user", "taun": "1[s]", "taup": "1[s]", "selection": {"all": true}})");
        const Run r = run(modelOf(junction(QStringLiteral("5"), QStringLiteral("1e16[1/cm^3]"), QStringLiteral("1e16[1/cm^3]"), QStringLiteral("0.4[V]"),
                                           QStringLiteral(R"({"type": "stationary"})"), srh)));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.solution;
        const double I = global(s, QStringLiteral("semi.I_a"));
        const double W = std::sqrt(2 * es * (2 * N) * (vbi - 0.4 - 2 * vt) / (Q * N * N));
        const double wq = 5e-6 - W / 2;
        const double J0 = Q * ni * ni * (0.14 * vt / (N * wq) + 0.045 * vt / (N * wq));
        const double expected = J0 * std::expm1(0.4 / vt) * 1e-6;
        qInfo("diode at 0.4 V: I %.5g A (Shockley's %.5g), into k %.5g; %s", I, expected, global(s, QStringLiteral("semi.I_k")),
              qPrintable(s.log.join(QLatin1String("; "))));
        QVERIFY2(std::abs(I / expected - 1) < 0.03, qPrintable(QString::number(I / expected)));
        QVERIFY(std::abs(I + global(s, QStringLiteral("semi.I_k"))) < 1e-6 * I);
    }

    void aMosCapacitor()
    {
        // 10 nm of oxide on p-type 1e17 cm⁻³, an n⁺ polysilicon gate: its
        // capacitance at flat band (Cox in series with εs/LD), deep in
        // inversion at 1 MHz (Cox with εs/Wmax: the minority carriers do not
        // follow) and at 1 µHz (they do: Cox again); in accumulation Cox.
        const double ni = siliconNi(), vt = KB * 300 / Q, Na = 1e23, es = 11.7 * Eps0, cox = 3.9 * Eps0 / 10e-9;
        const double dEc = 1.12 * Q / 2 + KB * 300 / 2 * std::log(2.8 / 1.04);
        const double vfb = (4.05 * Q - 4.05 * Q - dEc) / Q - vt * std::log(Na / ni);
        const double LD = std::sqrt(es * vt / (Q * Na)), cfb = cox * (es / LD) / (cox + es / LD);
        const double wmax = std::sqrt(4 * es * vt * std::log(Na / ni) / (Q * Na)), cmin = cox * (es / wmax) / (cox + es / wmax);
        auto mos = [&](double vg, const QString& f) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "bulk", "position": ["0", "-1"], "size": ["0.1", "0.8"]},
                             {"type": "rectangle", "name": "surface", "position": ["0", "-0.2"], "size": ["0.1", "0.2"]},
                             {"type": "rectangle", "name": "oxide", "size": ["0.1", "0.01"]}],
                "materials": [{"material": "Silicon", "selection": {"only": ["bulk", "surface"]}},
                              {"material": "Silicon dioxide", "selection": {"only": ["oxide"]}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "doping", "dopant": "acceptor", "N0": "1e17[1/cm^3]", "selection": {"only": ["bulk", "surface"]}},
                  {"type": "srh", "selection": {"all": true}},
                  {"type": "gate", "name": "g", "V0": "%1[V]", "phim": "4.05[eV]", "selection": {"of": ["oxide"], "side": "top"}},
                  {"type": "ohmic", "name": "b", "V0": "0[V]", "selection": {"of": ["bulk"], "side": "bottom"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "%3", "features": [
                  {"type": "size", "level": "domain", "selection": {"only": ["surface", "oxide"]}, "hmax": "%4"}]}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}, {"type": "frequency", "frequencies": "%2"}]}]})json")
                .arg(vg)
                .arg(f)
                .arg(slowBuild() ? QStringLiteral("0.05") : QStringLiteral("0.02"))
                .arg(slowBuild() ? QStringLiteral("0.006") : QStringLiteral("0.003"))
                .toUtf8();
        };
        // (Each per area: 0.1 µm by 1 m. Low frequency: the quasi-static C0 at
        // equilibrium - the small-signal system at 1 µHz is singular to within
        // a double's digits, the inversion layer held by almost nothing.)
        struct Cs {
            double hf, lf;
        };
        auto capacitance = [&](double vg) {
            const Run r = run(modelOf(mos(vg, QStringLiteral("1[MHz]"))));
            if (!r.error.isEmpty()) qFatal("%s", qPrintable(r.error));
            return Cs{global(*r.solution, QStringLiteral("semi.Cgg")) / 1e-7, global(*r.solution, QStringLiteral("semi.C0gg")) / 1e-7};
        };
        const Cs atFlat = capacitance(vfb), inverted = capacitance(vfb + 2.5), accumulated = capacitance(vfb - 2);
        const double flat = atFlat.hf, hf = inverted.hf, lf = inverted.lf, acc = accumulated.hf;
        // (At flat band and in accumulation the majority carriers follow at
        // any frequency: C0 and the 1 MHz capacitance agree.)
        QVERIFY2(std::abs(atFlat.lf / atFlat.hf - 1) < 1e-3, qPrintable(QStringLiteral("%1 %2").arg(atFlat.lf).arg(atFlat.hf)));
        QVERIFY(std::abs(accumulated.lf / accumulated.hf - 1) < 1e-3);
        qInfo("MOS: Cox %.4g F/m²; flat band at %.3f V: %.4g (%.4g); inversion %.4g at 1 MHz (Cmin %.4g), %.4g quasi-static; accumulation %.4g",
              cox, vfb, flat, cfb, hf, cmin, lf, acc);
        QVERIFY2(std::abs(flat / cfb - 1) < 0.03, qPrintable(QString::number(flat / cfb)));
        QVERIFY2(std::abs(hf / cmin - 1) < 0.08, qPrintable(QString::number(hf / cmin)));
        QVERIFY(lf > 0.9 * cox && lf < cox);
        QVERIFY(acc > 0.9 * cox && acc < cox);
    }

    void mobilityModels()
    {
        // The bar again: by doping (Caughey and Thomas at 1e17 cm⁻³), and
        // velocity saturation at 1e7 V/m (µE/vsat 14: v = µE / √(1 + (µE/vsat)²)).
        // (Both uniform along the bar: a sanitized build's meshes coarser.)
        const double ct = 68.5 + (1400 - 68.5) / (1 + std::pow(1e17 / 9.2e16, 0.711));   // cm²/Vs
        const Run doped = run(modelOf(bar(QStringLiteral("10"),
                                          QStringLiteral(R"({"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": true}},
                                                             {"type": "model", "mobility": "doping", "selection": {"all": true}},)"),
                                          QStringLiteral("0.1[V]"), {}, slowBuild() ? QStringLiteral("0.25") : QStringLiteral("0.1"))));
        QVERIFY2(doped.error.isEmpty(), qPrintable(doped.error));
        const double I1 = global(*doped.solution, QStringLiteral("semi.I_2")), expected1 = Q * 1e23 * ct * 1e-4 * 0.1 * 1e-6 / 1e-5;
        const Run fast = run(modelOf(bar(QStringLiteral("1"),
                                         QStringLiteral(R"({"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"all": true}},
                                                            {"type": "model", "saturation": true, "selection": {"all": true}},)"),
                                         QStringLiteral("10[V]"), {}, slowBuild() ? QStringLiteral("0.125") : QStringLiteral("0.05"))));
        QVERIFY2(fast.error.isEmpty(), qPrintable(fast.error));
        const double muE = 0.14 * 1e7, v = muE / std::sqrt(1 + (muE / 1e5) * (muE / 1e5));
        const double I2 = global(*fast.solution, QStringLiteral("semi.I_2")), expected2 = Q * 1e22 * v * 1e-6;
        // (What is read at a point the mobility as solved: the current
        // density across the bar its current's, semi.mun saturated - both
        // were the low-field mobility's, 14 times more.)
        const double J2 = at(*fast.solution, QStringLiteral("semi.normJn"), {0.5, 0.5}) * 1e-6;
        const double mu2 = at(*fast.solution, QStringLiteral("semi.mun"), {0.5, 0.5});
        qInfo("mobility: by doping %.5g A (%.5g, µ %.1f cm²/Vs); saturated %.5g A (q n v %.5g; the density across %.5g, µ %.1f cm²/Vs)", I1,
              expected1, ct, I2, expected2, J2, mu2 * 1e4);
        QVERIFY2(std::abs(I1 / expected1 - 1) < 3e-3, qPrintable(QString::number(I1 / expected1)));
        QVERIFY2(std::abs(I2 / expected2 - 1) < 0.03, qPrintable(QString::number(I2 / expected2)));
        QVERIFY2(std::abs(J2 / std::abs(I2) - 1) < 0.03, qPrintable(QString::number(J2 / I2)));
        QVERIFY2(std::abs(mu2 * 1e7 / v - 1) < 0.03, qPrintable(QString::number(mu2 * 1e7 / v)));
    }

    void masettiMobility()
    {
        // Masetti's mobility by doping, silicon's figures (its defaults):
        // electrons at 1e17 cm⁻³ (Pc 0: Lumerical's form) and at 1e20 (where
        // its µ1 term, of Cs, counts: 5 %), holes at 1e17 (Pc 9.23e16 cm⁻³:
        // e^(-Pc/N) counts) - the bar's current and semi.mun, semi.mup at
        // its middle.
        auto masetti = [](double N, double m1, double m2, double mu1, double pc, double cr, double cs, double a, double b, double mu) {
            return m1 * std::exp(-pc / N) + (mu - m2) / (1 + std::pow(N / cr, a)) - mu1 / (1 + std::pow(cs / N, b));
        };
        struct Case {
            const char* dopant;
            double N, mu;   // (cm⁻³, Masetti's µ)
        };
        const Case cases[] = {{"donor", 1e17, masetti(1e17, 52.2, 52.2, 43.4, 0, 9.68e16, 3.43e20, 0.68, 2, 1400)},
                              {"donor", 1e20, masetti(1e20, 52.2, 52.2, 43.4, 0, 9.68e16, 3.43e20, 0.68, 2, 1400)},
                              {"acceptor", 1e17, masetti(1e17, 44.9, 0, 29.0, 9.23e16, 2.23e17, 6.1e20, 0.719, 2, 450)}};
        const QString hmax = slowBuild() ? QStringLiteral("0.25") : QStringLiteral("0.1");
        for (const Case& c : cases) {
            const bool n = QLatin1String(c.dopant) == QLatin1String("donor");
            const Run r = run(modelOf(bar(QStringLiteral("10"),
                                          QStringLiteral(R"({"type": "doping", "dopant": "%1", "N0": "%2[1/cm^3]", "selection": {"all": true}},
                                                            {"type": "model", "mobility": "masetti", "selection": {"all": true}},)")
                                              .arg(QLatin1String(c.dopant))
                                              .arg(c.N),
                                          QStringLiteral("0.1[V]"), {}, hmax)));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double got = at(*r.solution, n ? QStringLiteral("semi.mun") : QStringLiteral("semi.mup"), {5, 0.5}) * 1e4;
            const double current = std::abs(global(*r.solution, QStringLiteral("semi.I_2"))) / (Q * c.N * 1e6 * 1e-4 * 0.1 * 1e-6 / 1e-5);
            qInfo("Masetti, %s at %g cm⁻³: %.4f cm²/Vs (%.4f), from the current %.4f", c.dopant, c.N, got, c.mu, current);
            QVERIFY2(std::abs(got / c.mu - 1) < 1e-6, qPrintable(QString::number(got)));
            QVERIFY2(std::abs(current / c.mu - 1) < 3e-3, qPrintable(QString::number(current)));
        }
    }

    void saturationExponents()
    {
        // µ0 / (1 + (µ0 F/vsat)^β)^(1/β) at 1e7 V/m: the electrons' β 1
        // (theirs 2 by default), the holes' 1 by default and 2. (The field
        // uniform: a sanitized build's mesh coarse, the holes' default left
        // out - three solves near QtTest's 300 s there.)
        struct Case {
            const char* dopant;
            const char* extra;
            double mu, vsat, beta;
        };
        const Case cases[] = {{"donor", R"(, "betan": "1")", 0.14, 1e5, 1}, {"acceptor", R"(, "betap": "2")", 0.045, 8e4, 2}, {"acceptor", "", 0.045, 8e4, 1}};
        for (const Case& c : cases) {
            if (slowBuild() && &c == &cases[2]) continue;
            const Run r = run(modelOf(bar(QStringLiteral("1"),
                                          QStringLiteral(R"({"type": "doping", "dopant": "%1", "N0": "1e16[1/cm^3]", "selection": {"all": true}},
                                                            {"type": "model", "saturation": true%2, "selection": {"all": true}},)")
                                              .arg(QLatin1String(c.dopant), QLatin1String(c.extra)),
                                          QStringLiteral("10[V]"), {}, slowBuild() ? QStringLiteral("0.25") : QStringLiteral("0.05"))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double muF = c.mu * 1e7, v = muF / std::pow(1 + std::pow(muF / c.vsat, c.beta), 1 / c.beta);
            const double I = std::abs(global(*r.solution, QStringLiteral("semi.I_2"))), expected = Q * 1e22 * v * 1e-6;
            qInfo("saturation, %s, β %g: I %.5g A (q N v %.5g)", c.dopant, c.beta, I, expected);
            QVERIFY2(std::abs(I / expected - 1) < 0.03, qPrintable(QString::number(I / expected)));
        }
    }

    void srhLifetimes()
    {
        // The lifetimes by doping (Fossum's τ0 / (A + B N/N0 + C (N/N0)^E),
        // their own N0s) of a τ0 of x; semi.taun, semi.taup at the middle.
        {
            const Run r = run(modelOf(bar(QStringLiteral("10"),
                                          QString::fromUtf8(R"x({"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": true}},
                                                {"type": "srh", "source": "user", "taun": "1[us]*(1 + x/10[um])", "taup": "2[us]", "dependence": "doping",
                                                 "N0n": "1e16[1/cm^3]", "N0p": "2e16[1/cm^3]", "fA": "1", "fB": "0.3", "fC": "2", "fE": "0.7", "selection": {"all": true}},)x"),
                                          QStringLiteral("0[V]"), {}, QStringLiteral("0.1"))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double tn = at(*r.solution, QStringLiteral("semi.taun"), {5, 0.5}), tp = at(*r.solution, QStringLiteral("semi.taup"), {5, 0.5});
            const double en = 1.5e-6 / (1 + 0.3 * 10 + 2 * std::pow(10, 0.7)), ep = 2e-6 / (1 + 0.3 * 5 + 2 * std::pow(5, 0.7));
            qInfo("lifetimes by doping: τn %.5g s (%.5g), τp %.5g (%.5g)", tn, en, tp, ep);
            QVERIFY2(std::abs(tn / en - 1) < 5e-3, qPrintable(QString::number(tn / en)));
            QVERIFY2(std::abs(tp / ep - 1) < 1e-9, qPrintable(QString::number(tp / ep)));
        }
        // Trap-assisted tunnelling (Hurkx's) in a uniform 2e7 V/m: τ/(1 + Γ),
        // Γ = ∫ e^(u - K u^(3/2)) du to ΔE/kT, K = 4/3 √(2 m*) (kT)^(3/2)/(q ħ F),
        // ΔE the trap's depth below Ec (above Ev) - integrated here on its own.
        // (A sanitized build: the field uniform, its mesh coarse; without the
        // tunnelling left out.)
        for (const bool tat : {true, false}) {
            if (slowBuild() && !tat) continue;
            const Run r = run(modelOf(bar(QStringLiteral("1"),
                                          QStringLiteral(R"({"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"all": true}},
                                                            {"type": "srh", "source": "user", "taun": "1[us]", "taup": "1[us]", "tat": %1, "mtn": "0.25", "mtp": "0.3",
                                                             "selection": {"all": true}},)")
                                              .arg(tat ? QStringLiteral("true") : QStringLiteral("false")),
                                          QStringLiteral("20[V]"), {}, slowBuild() ? QStringLiteral("0.25") : QStringLiteral("0.05"))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.solution;
            const QPointF mid(0.5, 0.5);
            const double F = at(s, QStringLiteral("semi.normE"), mid);
            const double Ec = at(s, QStringLiteral("semi.Ec"), mid), Ev = at(s, QStringLiteral("semi.Ev"), mid), Ei = at(s, QStringLiteral("semi.Ei"), mid);
            const double kT = KB * 300, hbar = 1.054571817e-34, m0 = 9.1093837015e-31;
            auto gamma = [&](double dE, double mt) {
                const double K = 4.0 / 3 * std::sqrt(2 * mt * m0) * std::pow(kT, 1.5) / (Q * hbar * F), top = dE / kT;
                const int n = 200000;
                double sum = 0;
                for (int i = 0; i <= n; ++i) {
                    const double u = top * i / n;
                    sum += (i == 0 || i == n ? 0.5 : 1.0) * std::exp(u - K * std::pow(u, 1.5));
                }
                return sum * top / n;
            };
            const double gn = tat ? gamma(Ec - Ei, 0.25) : 0, gp = tat ? gamma(Ei - Ev, 0.3) : 0;
            const double tn = at(s, QStringLiteral("semi.taun"), mid), tp = at(s, QStringLiteral("semi.taup"), mid);
            qInfo("trap-assisted tunnelling %s: F %.4g V/m, Γn %.4f, Γp %.4f; τn %.5g s (%.5g), τp %.5g (%.5g)", tat ? "on" : "off", F, gn, gp, tn, 1e-6 / (1 + gn),
                  tp, 1e-6 / (1 + gp));
            QVERIFY(std::abs(F / 2e7 - 1) < 1e-3);
            QVERIFY2(std::abs(tn * (1 + gn) / 1e-6 - 1) < 1e-3, qPrintable(QString::number(tn * (1 + gn) / 1e-6)));
            QVERIFY2(std::abs(tp * (1 + gp) / 1e-6 - 1) < 1e-3, qPrintable(QString::number(tp * (1 + gp) / 1e-6)));
            if (tat) QVERIFY(gn > 0.05 && gp > 0.05);
        }
    }

    void surfaceRecombination()
    {
        // n-type, 1e16 cm⁻³, a uniform generation G (low injection: Δp 1e-3
        // of n); its left end a surface of recombination velocity S (the
        // holes', 1e4 cm/s; the electrons' ten times that, all but unfelt in
        // n-type), its right an ohmic contact 60 µm away: the holes' Δp = G τ
        // + A e^(-x/L) + B e^(x/L), D Δp'(0) = S Δp(0), Δp(60 µm) = 0 (L =
        // √(D τ), D = kT/q µp).
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um",
            "geometry": [{"type": "rectangle", "name": "bar", "size": ["60", "1"]}],
            "materials": [{"material": "Silicon", "selection": {"all": true}}],
            "physics": [{"type": "semiconductor", "tag": "semi", "features": [
              {"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"all": true}},
              {"type": "srh", "source": "user", "taun": "0.1[us]", "taup": "0.1[us]", "selection": {"all": true}},
              {"type": "generation", "G": "1e26[1/(m^3*s)]", "selection": {"all": true}},
              {"type": "surfacerec", "Sn": "1e5[cm/s]", "Sp": "1e4[cm/s]", "selection": {"of": ["bar"], "side": "left"}},
              {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "right"}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "0.5"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json";
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.solution;
        const double G = 1e26, tau = 1e-7, S = 100, D = KB * 300 / Q * 0.045, L = std::sqrt(D * tau), W = 60e-6;
        // (A, B from the two ends: (D/L + S) A + (S - D/L) B = -S G τ; A e^(-W/L) + B e^(W/L) = -G τ.)
        const double a11 = D / L + S, a12 = S - D / L, b1 = -S * G * tau, a21 = std::exp(-W / L), a22 = std::exp(W / L), b2 = -G * tau;
        const double det = a11 * a22 - a12 * a21, A = (b1 * a22 - a12 * b2) / det, B = (a11 * b2 - a21 * b1) / det;
        auto expected = [&](double x) { return G * tau + A * std::exp(-x / L) + B * std::exp(x / L); };
        for (double x : {0.0, 5.0, 30.0}) {
            const QPointF p(x, 0.5);
            const double n = at(s, QStringLiteral("semi.n"), p), ni = at(s, QStringLiteral("semi.ni"), p);
            const double dp = at(s, QStringLiteral("semi.p"), p) - ni * ni / n, e = expected(x * 1e-6);
            qInfo("surface recombination: Δp at %g µm %.5g (%.5g; G τ %.3g)", x, dp, e, G * tau);
            QVERIFY2(std::abs(dp / e - 1) < 5e-3, qPrintable(QStringLiteral("%1 µm: %2").arg(x).arg(dp / e)));
        }
    }

    void dopingAndRecombination()
    {
        // Doping of x (an expression); direct recombination alone in n-type
        // under a uniform generation: Δp = G / (B n), far from the contacts.
        // (B large: the diffusion length 2.8 µm, the contacts 20 µm away.)
        const QString extra = QString::fromUtf8(R"x(, {"type": "radiative", "B": "1e-8[cm^3/s]", "selection": {"all": true}},
            {"type": "generation", "G": "1e27[1/(m^3*s)]", "selection": {"all": true}})x");
        const Run r = run(modelOf(bar(QStringLiteral("40"),
                                      QString::fromUtf8(R"x({"type": "doping", "dopant": "donor", "profile": "expression", "N": "1e16[1/cm^3]*(1 + x/40[um])", "selection": {"all": true}},)x"),
                                      QStringLiteral("0[V]"), extra, QStringLiteral("0.5"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.solution;
        const double nd = at(s, QStringLiteral("semi.Nd"), {20, 0.5});
        const double n = at(s, QStringLiteral("semi.n"), {20, 0.5});
        const double dp = at(s, QStringLiteral("semi.p"), {20, 0.5}) - siliconNi() * siliconNi() / n;
        const double expected = 1e27 / (1e-14 * n);
        qInfo("doping and recombination: Nd %.5g at 20 µm (1.5e22), Δp %.5g (G/(B n) %.5g)", nd, dp, expected);
        QVERIFY(std::abs(nd / 1.5e22 - 1) < 1e-3);
        QVERIFY2(std::abs(dp / expected - 1) < 0.01, qPrintable(QString::number(dp / expected)));
    }

    void aSchottkyDiode()
    {
        // n-type 1e16 cm⁻³, a 0.6 eV barrier at the left: reversed, the current
        // thermionic emission's saturation q vn Nc e^(-φB/kT) (the drift through
        // the depletion faster); forward, rising as e^(V/Vt).
        auto diode = [](double va) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "bar", "size": ["2", "1"]}],
                "materials": [{"material": "Silicon", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"all": true}},
                  {"type": "schottky", "name": "a", "V0": "%1[V]", "phiB": "0.6[eV]", "selection": {"of": ["bar"], "side": "left"}},
                  {"type": "ohmic", "name": "k", "selection": {"of": ["bar"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "%2"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                .arg(va)
                .arg(slowBuild() ? QStringLiteral("0.05") : QStringLiteral("0.025"))
                .toUtf8();
        };
        const double vt = KB * 300 / Q;
        const double js = Q * 1e5 * 2.8e25 * std::exp(-0.6 / vt) * 1e-6;
        double I[3];
        const double va[3] = {-0.5, 0.1, 0.15};
        for (int k = 0; k < 3; ++k) {
            const Run r = run(modelOf(diode(va[k])));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            I[k] = global(*r.solution, QStringLiteral("semi.I_a"));
        }
        const double ideality = 0.05 / (vt * std::log(I[2] / I[1]));
        qInfo("Schottky: %.4g A reversed (q vn Nc e^-φB/kT %.4g), %.4g and %.4g forward (ideality %.3f)", I[0], -js, I[1], I[2], ideality);
        QVERIFY2(std::abs(I[0] / -js - 1) < 0.15, qPrintable(QString::number(I[0] / -js)));
        QVERIFY(ideality > 0.98 && ideality < 1.1);
    }

    void fixedOxideCharge()
    {
        // q × 1e11 cm⁻² at the oxide's interface shifts the flat band by -Qf/Cox:
        // there the capacitance is the flat band's again.
        auto mos = [](double vg, bool charged) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "bulk", "position": ["0", "-1"], "size": ["0.1", "0.8"]},
                             {"type": "rectangle", "name": "surface", "position": ["0", "-0.2"], "size": ["0.1", "0.2"]},
                             {"type": "rectangle", "name": "oxide", "size": ["0.1", "0.01"]}],
                "materials": [{"material": "Silicon", "selection": {"only": ["bulk", "surface"]}},
                              {"material": "Silicon dioxide", "selection": {"only": ["oxide"]}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "doping", "dopant": "acceptor", "N0": "1e17[1/cm^3]", "selection": {"only": ["bulk", "surface"]}},
                  {"type": "gate", "name": "g", "V0": "%1[V]", "phim": "4.05[eV]", "selection": {"of": ["oxide"], "side": "top"}},
                  {"type": "ohmic", "name": "b", "selection": {"of": ["bulk"], "side": "bottom"}} %2]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.05", "features": [
                  {"type": "size", "level": "domain", "selection": {"only": ["surface", "oxide"]}, "hmax": "0.005"}]}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                .arg(vg, 0, 'g', 12)
                .arg(charged ? QStringLiteral(R"(, {"type": "charge", "rhos": "1.602176634e-19[C]*1e11[1/cm^2]", "selection": {"between": [["surface"], ["oxide"]]}})")
                             : QString())
                .toUtf8();
        };
        const double vt = KB * 300 / Q, ni = siliconNi(), Na = 1e23, cox = 3.9 * Eps0 / 10e-9;
        const double dEc = 1.12 * Q / 2 + KB * 300 / 2 * std::log(2.8 / 1.04);
        const double vfb = -dEc / Q - vt * std::log(Na / ni), shift = -Q * 1e15 / cox;
        const Run plain = run(modelOf(mos(vfb, false)));
        QVERIFY2(plain.error.isEmpty(), qPrintable(plain.error));
        const Run charged = run(modelOf(mos(vfb + shift, true)));
        QVERIFY2(charged.error.isEmpty(), qPrintable(charged.error));
        const double c0 = global(*plain.solution, QStringLiteral("semi.C0gg")), c1 = global(*charged.solution, QStringLiteral("semi.C0gg"));
        qInfo("fixed charge: the flat band moved %.4f V; C0 there %.6g F (%.6g without the charge, at flat band)", shift, c1, c0);
        QVERIFY2(std::abs(c1 / c0 - 1) < 2e-3, qPrintable(QString::number(c1 / c0)));
    }

    // A semiconductor's models take its material's parameters: GaAs's own
    // Caughey and Thomas (Lumerical's: µmin 40, Nref 8.1e16 cm⁻³, α 1), not
    // silicon's; one given on the feature wins; where its material gives
    // none - Masetti's on GaAs - silicon's are taken and the log says so.
    void materialModelSets()
    {
        auto gaas = [](const QString& model) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "bar", "size": ["10", "1"]}],
                "materials": [{"material": "Gallium arsenide", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "model", "selection": {"all": true} %1},
                  {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": true}},
                  {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}},
                  {"type": "ohmic", "name": "2", "V0": "0.01[V]", "selection": {"of": ["bar"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.5"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                .arg(model)
                .toUtf8();
        };
        const Run own = run(modelOf(gaas(QStringLiteral(R"(, "mobility": "doping")"))));
        QVERIFY2(own.error.isEmpty(), qPrintable(own.error));
        const double mu = at(*own.solution, QStringLiteral("semi.mun"), {5, 0.5});
        QVERIFY2(std::abs(mu / ((40 + (8500 - 40) / (1 + 1e17 / 8.1e16)) * 1e-4) - 1) < 1e-9, qPrintable(QString::number(mu)));
        QVERIFY2(!own.solution->log.join(QLatin1Char('\n')).contains(QLatin1String("silicon's")), qPrintable(own.solution->log.join(QLatin1Char('\n'))));
        const Run set = run(modelOf(gaas(QStringLiteral(R"(, "mobility": "doping", "munmin": "100[cm^2/(V*s)]")"))));
        QVERIFY2(set.error.isEmpty(), qPrintable(set.error));
        QVERIFY(std::abs(at(*set.solution, QStringLiteral("semi.mun"), {5, 0.5}) / ((100 + (8500 - 100) / (1 + 1e17 / 8.1e16)) * 1e-4) - 1) < 1e-9);
        const Run masetti = run(modelOf(gaas(QStringLiteral(R"(, "mobility": "masetti")"))));
        QVERIFY2(masetti.error.isEmpty(), qPrintable(masetti.error));
        const QString log = masetti.solution->log.join(QLatin1Char('\n'));
        QVERIFY2(log.contains(QLatin1String("Gallium arsenide (bar): its material gives no parameters for Masetti's mobility - silicon's")), qPrintable(log));
    }

    // At its temperature: silicon at 400 K - its gap Varshni's, Nc and Nv
    // (T/300 K)^1.5 (semi.ni), its mobilities (T/300 K)^-2.5 (semi.mun) -,
    // its material's texts read at the physics' T.
    void atItsTemperature()
    {
        QByteArray json = bar(QStringLiteral("10"), QStringLiteral(R"({"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"all": true}},)"),
                              QStringLiteral("0.01[V]"), {}, QStringLiteral("0.5"));
        json.replace(R"("tag": "semi", )", R"("tag": "semi", "T": "400[K]", )");
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const double T = 400, t = T / 300, eg = 1.12 - 4.73e-4 * (T * T / (T + 636) - 9e4 / 936);
        const double ni = std::sqrt(2.8e25 * 1.04e25) * std::pow(t, 1.5) * std::exp(-eg * Q / (2 * KB * T));
        QVERIFY2(std::abs(at(*r.solution, QStringLiteral("semi.ni"), {5, 0.5}) / ni - 1) < 1e-6, qPrintable(QString::number(at(*r.solution, QStringLiteral("semi.ni"), {5, 0.5}) / ni)));
        QVERIFY(std::abs(at(*r.solution, QStringLiteral("semi.mun"), {5, 0.5}) / (0.14 * std::pow(t, -2.5)) - 1) < 1e-9);
        // A lattice temperature TL scales them from the physics' T: TL 400 K
        // there, as without it; 500 K, (5/4)^-2.5 more.
        for (const double TL : {400.0, 500.0}) {
            QByteArray lattice = json;
            lattice.replace(R"("T": "400[K]", )", QStringLiteral(R"("T": "400[K]", "TL": "%1[K]", )").arg(TL).toUtf8());
            const Run l = run(modelOf(lattice));
            QVERIFY2(l.error.isEmpty(), qPrintable(l.error));
            const double mu = at(*l.solution, QStringLiteral("semi.mun"), {5, 0.5}), expected = 0.14 * std::pow(TL / 300, -2.5);
            QVERIFY2(std::abs(mu / expected - 1) < 1e-9, qPrintable(QStringLiteral("%1 %2").arg(mu).arg(expected)));
        }
    }

    // A gate's and a Schottky contact's metal beside it (gold): its work
    // function the library's, 5.1 eV, where none is set - the solution as
    // with it set, the log saying so; a gate with no metal beside it 4.05 eV.
    void metalWorkFunction()
    {
        auto mos = [](const QString& gate, const QString& metal) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "si", "position": ["0", "-1"], "size": ["1", "1"]},
                             {"type": "rectangle", "name": "oxide", "size": ["1", "0.01"]},
                             {"type": "rectangle", "name": "metal", "position": ["0", "0.01"], "size": ["1", "0.05"]}],
                "materials": [{"material": "Silicon", "selection": {"only": ["si"]}}, {"material": "Silicon dioxide", "selection": {"only": ["oxide"]}},
                              {"material": "%2", "selection": {"only": ["metal"]}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "selection": {"objects": ["si", "oxide"]}, "features": [
                  {"type": "doping", "dopant": "acceptor", "N0": "1e17[1/cm^3]", "selection": {"only": ["si"]}},
                  {"type": "ohmic", "name": "b", "V0": "0[V]", "selection": {"of": ["si"], "side": "bottom"}},
                  {"type": "gate", "name": "g", "V0": "0[V]" %1, "selection": {"between": [["oxide"], ["metal"]]}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.05", "features": [{"type": "size", "level": "domain", "selection": {"only": ["oxide"]}, "hmax": "0.005"}]}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                .arg(gate, metal)
                .toUtf8();
        };
        const Run beside = run(modelOf(mos(QString(), QStringLiteral("Gold"))));
        QVERIFY2(beside.error.isEmpty(), qPrintable(beside.error));
        const Run given = run(modelOf(mos(QStringLiteral(R"(, "phim": "5.1[eV]")"), QStringLiteral("Gold"))));
        const Run none = run(modelOf(mos(QString(), QStringLiteral("Air"))));
        QVERIFY2(given.error.isEmpty() && none.error.isEmpty(), qPrintable(given.error + none.error));
        const double qb = global(*beside.solution, QStringLiteral("semi.Q_g")), qg = global(*given.solution, QStringLiteral("semi.Q_g")), q0 = global(*none.solution, QStringLiteral("semi.Q_g"));
        qInfo("gate's charge: gold beside it %g C/m, 5.1 eV given %g, nothing beside it %g", qb, qg, q0);
        QVERIFY2(qb == qg && std::abs(qb - q0) > 0.1 * std::abs(q0), qPrintable(QStringLiteral("%1 %2 %3").arg(qb).arg(qg).arg(q0)));
        QVERIFY2(beside.solution->log.join(QLatin1Char('\n')).contains(QLatin1String("its work function its metal's, Gold: 5.1 eV")),
                 qPrintable(beside.solution->log.join(QLatin1Char('\n'))));
        // A Schottky contact on n-silicon: its barrier gold's less χ, 1.05 eV.
        auto schottky = [](const QString& barrier) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "si", "size": ["2", "1"]}, {"type": "rectangle", "name": "metal", "position": ["2", "0"], "size": ["0.2", "1"]}],
                "materials": [{"material": "Silicon", "selection": {"only": ["si"]}}, {"material": "Gold", "selection": {"only": ["metal"]}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "selection": {"objects": ["si"]}, "features": [
                  {"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"only": ["si"]}},
                  {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["si"], "side": "left"}},
                  {"type": "schottky", "name": "a", "V0": "0.2[V]" %1, "selection": {"between": [["si"], ["metal"]]}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.05"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                .arg(barrier)
                .toUtf8();
        };
        const Run gold = run(modelOf(schottky(QString())));
        const Run typed = run(modelOf(schottky(QStringLiteral(R"(, "phiB": "1.05[eV]")"))));
        QVERIFY2(gold.error.isEmpty() && typed.error.isEmpty(), qPrintable(gold.error + typed.error));
        // (5.1 - 4.05 eV and 1.05 eV a rounding apart.)
        const double ig = global(*gold.solution, QStringLiteral("semi.I_a")), it = global(*typed.solution, QStringLiteral("semi.I_a"));
        QVERIFY2(std::abs(ig / it - 1) < 1e-9, qPrintable(QStringLiteral("%1 %2").arg(ig).arg(it)));
        QVERIFY2(gold.solution->log.join(QLatin1Char('\n')).contains(QString::fromUtf8("its barrier its metal's, Gold: φ 5.1 eV less χ 4.05 eV, 1.05 eV")),
                 qPrintable(gold.solution->log.join(QLatin1Char('\n'))));
    }

    // Stimulated recombination (Lumerical's laser model): above threshold a
    // share c of the domains' recombination over R0 = Jth/(q t), the same
    // throughout - semi.Istim its current, q c (∫R - R0 A) over its area;
    // below threshold none.
    void stimulatedRecombination()
    {
        auto diode = [](const QString& jth) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "pside", "size": ["1", "1"]}, {"type": "rectangle", "name": "nside", "position": ["1", "0"], "size": ["1", "1"]}],
                "materials": [{"material": "Gallium arsenide", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "doping", "dopant": "acceptor", "N0": "1e17[1/cm^3]", "selection": {"only": ["pside"]}},
                  {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"only": ["nside"]}},
                  {"type": "radiative", "selection": {"all": true}},
                  {"type": "stimulated", "c": "0.5", "Jth": "%1", "t": "2[um]", "selection": {"all": true}},
                  {"type": "ohmic", "name": "a", "V0": "1.25[V]", "selection": {"of": ["pside"], "side": "left"}},
                  {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["nside"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.05"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                .arg(jth)
                .toUtf8();
        };
        const double jth = 1e5;   // A/m²
        const Run lasing = run(modelOf(diode(QStringLiteral("1e5[A/m^2]"))));
        QVERIFY2(lasing.error.isEmpty(), qPrintable(lasing.error));
        Solution& s = *lasing.solution;
        Node rec;
        rec.type = QStringLiteral("integral");
        rec.props.insert(QStringLiteral("expression"), QStringLiteral("semi.R*1[m]"));
        rec.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}});
        const DerivedResult d = evaluateDerived(s, rec);
        QVERIFY2(!d.values.isEmpty(), qPrintable(d.error));
        const double area = 2e-12, R0 = jth / (Q * 2e-6);
        const double expected = Q * 0.5 * (d.values.front().value - R0 * area * 1), istim = global(s, QStringLiteral("semi.Istim"));
        qInfo("stimulated: semi.Istim %g A, q c (∫R - R0 A) %g A; the diode's %g A", istim, expected, global(s, QStringLiteral("semi.I_k")));
        QVERIFY2(expected > 0 && std::abs(istim / expected - 1) < 0.02, qPrintable(QStringLiteral("%1 %2").arg(istim).arg(expected)));
        const Run dark = run(modelOf(diode(QStringLiteral("1e12[A/m^2]"))));
        QVERIFY2(dark.error.isEmpty(), qPrintable(dark.error));
        QCOMPARE(global(*dark.solution, QStringLiteral("semi.Istim")), 0.0);
    }

    // Impact ionization's model "material" (its default now): GaAs's own
    // Selberherr coefficients, one range - its ionization integrals as with
    // them typed; silicon's another. Its n side at 1e17 cm⁻³ (the field some
    // 5e5 V/cm, above the default E0's 4e5): the one range kept beyond it, E0
    // set on the feature or not. (A slow build's mesh coarser: the integrals
    // compared on one mesh.)
    void impactFromItsMaterial()
    {
        auto diode = [](const QString& impact, const char* donors) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "pside", "size": ["0.5", "0.2"]}, {"type": "rectangle", "name": "nside", "position": ["0.5", "0"], "size": ["2", "0.2"]}],
                "materials": [{"material": "Gallium arsenide", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "doping", "dopant": "acceptor", "N0": "1e18[1/cm^3]", "selection": {"only": ["pside"]}},
                  {"type": "doping", "dopant": "donor", "N0": "%2[1/cm^3]", "selection": {"only": ["nside"]}},
                  {"type": "impact", "currents": false %1, "selection": {"all": true}},
                  {"type": "ohmic", "name": "a", "V0": "-10[V]", "selection": {"of": ["pside"], "side": "left"}},
                  {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["nside"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "%3"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                .arg(impact, QLatin1String(donors), slowBuild() ? QStringLiteral("0.1") : QStringLiteral("0.05"))
                .toUtf8();
        };
        auto integral = [&diode](const QString& impact, const char* donors) {
            const Run r = run(modelOf(diode(impact, donors)));
            return r.error.isEmpty() ? global(*r.solution, QStringLiteral("semi.In")) : NAN;
        };
        const QString typedCoefficients = QStringLiteral(R"(, "model": "user", "an": "2.99e5[1/cm]", "bn": "6.85e5[V/cm]", "an2": "2.99e5[1/cm]", "bn2": "6.85e5[V/cm]",
                                                             "ap": "2.22e5[1/cm]", "bp": "6.57e5[V/cm]", "ap2": "2.22e5[1/cm]", "bp2": "6.57e5[V/cm]", "E0": "1e30[V/m]",
                                                             "betan": "1.6", "betap": "1.75")");
        const double in = integral(QString(), "1e16"), it = integral(typedCoefficients, "1e16"), is = integral(QStringLiteral(R"(, "model": "silicon")"), "1e16");
        qInfo("GaAs's ionization integral at -10 V: %g (typed %g; silicon's coefficients %g)", in, it, is);
        QVERIFY2(in > 0 && std::abs(in / it - 1) < 1e-9 && std::abs(is / in - 1) > 0.1, qPrintable(QStringLiteral("%1 %2 %3").arg(in).arg(it).arg(is)));
        const double high = integral(QString(), "1e17"), highTyped = integral(typedCoefficients, "1e17");
        const double highE0 = integral(QStringLiteral(R"(, "E0": "4e5[V/cm]")"), "1e17");
        qInfo("its n side 1e17 cm^-3: %g (typed %g; E0 set %g)", high, highTyped, highE0);
        QVERIFY2(high > 0 && std::abs(high / highTyped - 1) < 1e-9 && std::abs(highE0 / highTyped - 1) < 1e-9,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(high).arg(highTyped).arg(highE0)));
    }

    void plasmaDispersion()
    {
        // A slab of silicon, donors 1e18 cm⁻³ (its electrons that many): its
        // mode with the free carriers' dispersion as with n given as n + Δn,
        // Δn = -8.8e-22 × 1e18; its loss from Δα = 8.5e-18 × 1e18 cm⁻¹.
        auto slab = [](const QString& wave, bool semi) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "oxide", "position": ["0", "-1.5"], "size": ["0.4", "3.22"]},
                             {"type": "rectangle", "name": "core", "size": ["0.4", "0.22"]}],
                "materials": [{"material": "Silicon dioxide", "selection": {"only": ["oxide"]}},
                              {"material": "Silicon", "selection": {"only": ["core"]}}],
                "physics": [%2
                  {"type": "modes", "tag": "emw", "features": [%1]}],
                "mesh": {"size": "normal", "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "0.03"}]}}],
              "studies": [{"type": "study", "steps": [%3{"type": "mode", "physics": ["emw"], "frequency": "1550[nm]", "modes": 1, "neff": "3"}]}]})json")
                .arg(wave,
                     semi ? QStringLiteral(R"({"type": "semiconductor", "tag": "semi", "selection": {"only": ["core"]}, "features": [
                              {"type": "doping", "dopant": "donor", "N0": "1e18[1/cm^3]", "selection": {"all": true}},
                              {"type": "ohmic", "name": "c", "selection": {"of": ["core"], "side": "left"}}]},)")
                          : QString(),
                     semi ? QStringLiteral(R"({"type": "stationary", "physics": ["semi"]}, )") : QString())
                .toUtf8();
        };
        const Run carriers = run(modelOf(slab(QStringLiteral(R"({"type": "wave", "plasma": true, "selection": {"only": ["core"]}})"), true)));
        QVERIFY2(carriers.error.isEmpty(), qPrintable(carriers.error));
        const double dn = -8.8e-22 * 1e18, dalpha = 8.5e-18 * 1e18 * 100;
        // (The library's silicon's index at 1550 nm: its Sellmeier fitted to
        // Green's and Li's, Li's 3.4757 there.)
        Scope at1550(&Scope::builtins());
        at1550.setConstant(QStringLiteral("lambda0"), 1.55e-6, Dim::length());
        const double n0 = evaluateConstant(findMaterial(QStringLiteral("Silicon"))->properties.value(QStringLiteral("n")).toString(), at1550, Dim()).value_or(NAN);
        QVERIFY2(std::abs(n0 - 3.4757) < 2e-4, qPrintable(QString::number(n0, 'g', 10)));
        const Run given = run(modelOf(slab(QString::fromUtf8(R"x({"type": "wave", "source": "user", "n": "%3 + (%1)", "kappa": "%2", "selection": {"only": ["core"]}})x")
                                               .arg(dn, 0, 'g', 17)
                                               .arg(dalpha * 1.55e-6 / (4 * M_PI), 0, 'g', 17)
                                               .arg(n0, 0, 'g', 17),
                                           false)));
        QVERIFY2(given.error.isEmpty(), qPrintable(given.error));
        Solution& a = *carriers.solution;
        Solution& b = *given.solution;
        a.select(a.snapshotCount() - 1);
        b.select(b.snapshotCount() - 1);
        const double na = global(a, QStringLiteral("emw.neff")), nb = global(b, QStringLiteral("emw.neff"));
        const double la = global(a, QStringLiteral("emw.loss")), lb = global(b, QStringLiteral("emw.loss"));
        qInfo("plasma dispersion: neff %.7f (given %.7f), loss %.4g dB/m (%.4g)", na, nb, la, lb);
        QVERIFY(std::abs(na - nb) < 2e-6);
        QVERIFY(std::abs(la / lb - 1) < 0.01);
    }

    // The library's indices of the wavelength: a mode at 1310 nm takes
    // silicon's and its oxide's there (as with them given), indium
    // arsenide's (fitted from 0.1 eV above its gap, 2.731 µm) said out of
    // its range; germanium's (0.3 to 14 µm) not.
    void indicesOfTheWavelength()
    {
        auto slab = [](const QString& core, const QString& wave) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "name": "oxide", "position": ["0", "-1.5"], "size": ["0.4", "3.22"]},
                             {"type": "rectangle", "name": "core", "size": ["0.4", "0.22"]}],
                "materials": [{"material": "Silicon dioxide", "selection": {"only": ["oxide"]}},
                              {"material": "%1", "selection": {"only": ["core"]}}],
                "physics": [{"type": "modes", "tag": "emw", "features": [%2]}],
                "mesh": {"size": "normal", "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "0.03"}]}}],
              "studies": [{"type": "study", "steps": [{"type": "mode", "physics": ["emw"], "frequency": "1310[nm]", "modes": 1, "neff": "3"}]}]})json")
                .arg(core, wave)
                .toUtf8();
        };
        Scope at1310(&Scope::builtins());
        at1310.setConstant(QStringLiteral("lambda0"), 1.31e-6, Dim::length());
        auto index = [&](const char* name) {
            return evaluateConstant(findMaterial(QLatin1String(name))->properties.value(QStringLiteral("n")).toString(), at1310, Dim()).value_or(NAN);
        };
        const double nsi = index("Silicon"), nox = index("Silicon dioxide");
        QVERIFY2(nsi > 3.50 && nsi < 3.51 && nox > 1.446 && nox < 1.448, qPrintable(QStringLiteral("%1 %2").arg(nsi).arg(nox)));
        const Run library = run(modelOf(slab(QStringLiteral("Silicon"), QString())));
        const Run given = run(modelOf(slab(QStringLiteral("Silicon"), QStringLiteral(R"({"type": "wave", "source": "user", "n": "%1", "selection": {"only": ["core"]}},
                                                                                         {"type": "wave", "source": "user", "n": "%2", "selection": {"only": ["oxide"]}})")
                                                                       .arg(nsi, 0, 'g', 17)
                                                                       .arg(nox, 0, 'g', 17))));
        QVERIFY2(library.error.isEmpty() && given.error.isEmpty(), qPrintable(library.error + given.error));
        Solution& a = *library.solution;
        Solution& b = *given.solution;
        a.select(a.snapshotCount() - 1);
        b.select(b.snapshotCount() - 1);
        const double na = global(a, QStringLiteral("emw.neff")), nb = global(b, QStringLiteral("emw.neff"));
        qInfo("at 1310 nm: neff %.7f (the indices given %.7f)", na, nb);
        QVERIFY(std::abs(na - nb) < 1e-9 && !a.log.join(QLatin1Char(' ')).contains(QLatin1String("holds from")));
        const Run inas = run(modelOf(slab(QStringLiteral("Indium arsenide"), QString())));
        QVERIFY2(inas.error.isEmpty(), qPrintable(inas.error));
        const QString log = inas.solution->log.join(QLatin1Char('\n'));
        QVERIFY2(log.contains(QString::fromUtf8("Warning: Indium arsenide's index holds from 2.238 to 20 µm (as its material gives it); here it is taken at 1.31 µm")), qPrintable(log));
        const Run ge = run(modelOf(slab(QStringLiteral("Germanium"), QString())));
        QVERIFY2(ge.error.isEmpty() && !ge.solution->log.join(QLatin1Char(' ')).contains(QLatin1String("holds from")), qPrintable(ge.error));
        // (Its index given on the feature: the user's, nothing said.)
        const Run geGiven = run(modelOf(slab(QStringLiteral("Germanium"), QStringLiteral(R"({"type": "wave", "source": "user", "n": "4.36", "selection": {"only": ["core"]}})"))));
        QVERIFY2(geGiven.error.isEmpty(), qPrintable(geGiven.error));
        QVERIFY2(!geGiven.solution->log.join(QLatin1Char(' ')).contains(QLatin1String("holds from")), qPrintable(geGiven.solution->log.join(QLatin1Char('\n'))));
    }

    void carriersInTime()
    {
        // n-type, a generation G switched on at 0: the holes in the middle
        // rise as G τ (1 - e^(-t/τ)) (low injection, far from the contacts:
        // 40 µm, the diffusion length 3.4 µm).
        const QString extra = QString::fromUtf8(R"x(, {"type": "srh", "source": "user", "taun": "10[ns]", "taup": "10[ns]", "selection": {"all": true}},
            {"type": "generation", "G": "1e27[1/(m^3*s)]*(t > 0)", "selection": {"all": true}})x");
        Model m = modelOf(bar(QStringLiteral("40"), QStringLiteral(R"({"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"all": true}},)"),
                              QStringLiteral("0[V]"), extra, QStringLiteral("0.5")));
        for (Node& step : m.find(m.studies().front()->tag)->children)
            if (step.type == QLatin1String("stationary")) {
                step.type = QStringLiteral("transient");
                step.props.insert(QStringLiteral("times"), slowBuild() ? QStringLiteral("range(0, 10[ns], 20[ns])") : QStringLiteral("range(0, 10[ns], 50[ns])"));
                step.props.insert(QStringLiteral("rtol"), QStringLiteral("1e-4"));
            }
        const Run r = run(m);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        const int outputs = slowBuild() ? 3 : 6;
        QCOMPARE(s.snapshotCount(), outputs);
        const double p0 = siliconNi() * siliconNi() / 1e22;
        QStringList each;
        for (int k = 1; k < outputs; ++k) {
            s.select(k);
            const double dp = at(s, QStringLiteral("semi.p"), {20, 0.5}) - p0;
            const double expected = 1e27 * 10e-9 * (1 - std::exp(-k));
            each << QStringLiteral("%1 (%2)").arg(dp, 0, 'g', 5).arg(expected, 0, 'g', 5);
            QVERIFY2(std::abs(dp / expected - 1) < 0.01, qPrintable(each.join(QLatin1String(", "))));
        }
        qInfo("in time: Δp %s; %s", qPrintable(each.join(QLatin1String(", "))), qPrintable(s.log.join(QLatin1String("; "))));
    }

    void theExamples()
    {
        // The semiconductors' examples, solved as they come (fewer points in a
        // sanitized build), against what they say.
        const QString dir = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/");
        auto load = [&](const QString& name) {
            QFile f(dir + name);
            if (!f.open(QIODevice::ReadOnly)) qFatal("no example %s", qPrintable(name));
            Model m = modelOf(f.readAll());
            m.fileName = dir + name;
            return m;
        };
        auto sweepOnly = [](Model& m, const QString& name, const QString& values, const QString& unit) {
            QJsonArray rows;
            rows.append(QJsonArray{name, values, unit});
            for (Node& step : m.find(m.studies().front()->tag)->children)
                if (step.type == QLatin1String("sweep")) step.props.insert(QStringLiteral("parameters"), rows);
        };
        const double vt = KB * 300 / Q, es = 11.7 * Eps0;
        QElapsedTimer timer;
        {
            // The diode: exp(V/Vt) forward; the depletion layer's capacitance reversed.
            timer.start();
            Model m = load(QStringLiteral("pn_diode.qfem"));
            if (slowBuild()) {
                sweepOnly(m, QStringLiteral("Va"), QStringLiteral("-2, 0.5, 0.6"), QStringLiteral("V"));
                Node* mesh = m.find(m.mesh().tag);
                for (Node& size : mesh->children) size.props.insert(QStringLiteral("hmax"), QStringLiteral("0.04"));
            }
            const Run r = run(m);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            auto point = [&](double va) -> Solution& {
                for (const auto& s : r.study.solutions)
                    if (std::abs(s->sweptValues.value(QStringLiteral("Va")) - va) < 1e-9) return *s;
                qFatal("no point at %g V", va);
            };
            const double i5 = global(point(0.5), QStringLiteral("semi.I_a")), i6 = global(point(0.6), QStringLiteral("semi.I_a"));
            const double ideality = 0.1 / (vt * std::log(i6 / i5));
            const double ni = siliconNi(), N = 1e22;
            const double vbi = vt * std::log(N * N / (ni * ni));
            const double W = std::sqrt(2 * es * 2 * N * (vbi + 2 - 2 * vt) / (Q * N * N));
            const double c2 = global(point(-2), QStringLiteral("semi.Ckk")), expected = es / W * 1e-10;
            qInfo("pn diode: I %.4g A at 0.5 V, %.4g at 0.6 V (ideality %.3f); C %.4g F at -2 V (ε/W %.4g); %lld ms", i5, i6, ideality, c2, expected,
                  timer.elapsed());
            QVERIFY(ideality > 0.98 && ideality < 1.1);
            QVERIFY(std::abs(c2 / expected - 1) < 0.03);
            QVERIFY(std::abs(global(point(-2), QStringLiteral("semi.I_a"))) < 1e-3 * i5);
        }
        {
            // The MOS capacitor: Cox at both ends at low frequency, Cmin at 1 MHz in inversion.
            timer.start();
            Model m = load(QStringLiteral("mos_capacitor.qfem"));
            if (slowBuild()) {
                sweepOnly(m, QStringLiteral("Vg"), QStringLiteral("-3, 3"), QStringLiteral("V"));
                Node* mesh = m.find(m.mesh().tag);
                mesh->props.insert(QStringLiteral("hmax"), QStringLiteral("0.05"));
                for (Node& size : mesh->children) size.props.insert(QStringLiteral("hmax"), QStringLiteral("0.006"));
            }
            const Run r = run(m);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double cox = 3.9 * Eps0 / 10e-9, area = 0.1e-6 * 100e-6, Na = 1e23, ni = siliconNi();
            const double wmax = std::sqrt(4 * es * vt * std::log(Na / ni) / (Q * Na)), cmin = cox * (es / wmax) / (cox + es / wmax);
            Solution& low = *r.study.solutions.front();
            Solution& high = *r.study.solutions.back();
            auto c = [&](Solution& s, const char* name) {
                s.select(s.snapshotCount() - 1);
                return global(s, QString::fromLatin1(name)) / area;
            };
            const double accLF = c(low, "semi.C0gg"), accHF = c(low, "semi.Cgg"), invLF = c(high, "semi.C0gg"), invHF = c(high, "semi.Cgg");
            qInfo("MOS capacitor: at -3 V %.3f, %.3f Cox; at 3 V %.3f (LF), %.3f (HF; Cmin %.3f) Cox; %d points, %lld ms", accLF / cox, accHF / cox,
                  invLF / cox, invHF / cox, cmin / cox, int(r.study.solutions.size()), timer.elapsed());
            QVERIFY(accLF > 0.9 * cox && accHF > 0.9 * cox && invLF > 0.9 * cox);
            QVERIFY(std::abs(invHF / cmin - 1) < 0.08);
        }
        if (!slowBuild()) {
            // The phase shifter: neff up and loss down as it is reversed; Vπ·L.
            // (In an optimized build only: its modes take minutes in a
            // sanitized one.)
            timer.start();
            const Run r = run(load(QStringLiteral("pn_phase_shifter.qfem")));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            std::vector<double> neff, loss;
            for (const auto& s : r.study.solutions) {
                s->select(s->snapshotCount() - 1);
                neff.push_back(global(*s, QStringLiteral("emw.neff")));
                loss.push_back(global(*s, QStringLiteral("emw.loss")));
            }
            const double dn = neff.back() - neff.front();
            const double vpil = 4 * 1.55e-6 / (2 * dn) * 100;   // V·cm
            QStringList each;
            for (std::size_t k = 0; k < neff.size(); ++k) each << QStringLiteral("%1 (%2 dB/cm)").arg(neff[k], 0, 'f', 6).arg(loss[k] / 100, 0, 'f', 2);
            qInfo("phase shifter: neff %s; Vπ·L %.2f V·cm; %lld ms", qPrintable(each.join(QLatin1String(", "))), vpil, timer.elapsed());
            for (std::size_t k = 1; k < neff.size(); ++k) {
                QVERIFY(neff[k] > neff[k - 1]);
                QVERIFY(loss[k] < loss[k - 1]);
            }
            QVERIFY2(vpil > 1 && vpil < 5, qPrintable(QString::number(vpil)));
        }
    }

};

QTEST_GUILESS_MAIN(TestFemSemiconductor)
#include "test_fem_semiconductor.moc"
