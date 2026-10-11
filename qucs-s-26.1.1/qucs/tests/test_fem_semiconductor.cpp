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
        qInfo("mobility: by doping %.5g A (%.5g, µ %.1f cm²/Vs); saturated %.5g A (q n v %.5g)", I1, expected1, ct, I2, expected2);
        QVERIFY2(std::abs(I1 / expected1 - 1) < 3e-3, qPrintable(QString::number(I1 / expected1)));
        QVERIFY2(std::abs(I2 / expected2 - 1) < 0.03, qPrintable(QString::number(I2 / expected2)));
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
        const Run given = run(modelOf(slab(QString::fromUtf8(R"x({"type": "wave", "source": "user", "n": "3.476 + (%1)", "kappa": "%2", "selection": {"only": ["core"]}})x")
                                               .arg(dn, 0, 'g', 17)
                                               .arg(dalpha * 1.55e-6 / (4 * M_PI), 0, 'g', 17),
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
