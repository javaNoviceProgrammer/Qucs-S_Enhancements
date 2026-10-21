/*
 * test_fem_photodiode_power.cpp - a photodiode under strong light: its
 *                                 contacts behind a circuit (a series
 *                                 resistance, a load, a bias tee, a
 *                                 capacitance across) at rest, in
 *                                 frequency and in time
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_problem.h"
#include "fem_results.h"

#include <QJsonObject>
#include <QtTest>

#include <cmath>
#include <complex>
#include <vector>

using namespace qucs_s::fem;

namespace {

constexpr double Q = 1.602176634e-19;

bool slowBuild()
{
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
    r.study = runStudy(model, r.parameters, g.topology, nullptr, QString(), {});
    if (!r.study.ok()) r.error = r.study.error;
    return r;
}

double global(const Solution& s, const QString& name)
{
    const std::optional<double> v = s.global(name);
    return v ? *v : NAN;
}

/// A silicon p-i-n along x (µm): p⁺ 0.3, i 2 (1e15 cm⁻³ donors), n⁺ 0.3,
/// 0.5 high and 10 µm long (the model's thickness), lit uniformly in its
/// i region (G0 pairs per m³ and s: about a µA per 6e29); its anode a at
/// its left at Va (\a anode its contact's more: a circuit), its cathode k
/// grounded at its right; \a parameters more, \a steps the study's.
QByteArray pin(const QString& anode, const QString& steps, const QString& parameters = QString())
{
    const QString hmax = slowBuild() ? QStringLiteral("0.1") : QStringLiteral("0.05");
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "Va", "expression": "-2[V]"}, {"name": "G0", "expression": "6e29[1/(m^3*s)]"} %3],
      "components": [{"unit": "um", "thickness": "10[um]",
        "geometry": [{"type": "rectangle", "label": "p", "position": ["0", "0"], "size": ["0.3", "0.5"]},
                     {"type": "rectangle", "label": "i", "position": ["0.3", "0"], "size": ["2", "0.5"]},
                     {"type": "rectangle", "label": "n", "position": ["2.3", "0"], "size": ["0.3", "0.5"]}],
        "materials": [{"material": "Silicon", "selection": {"all": true}}],
        "physics": [{"type": "semiconductor", "tag": "semi", "features": [
          {"type": "doping", "dopant": "acceptor", "N0": "1e19[1/cm^3]", "selection": {"only": ["p"]}},
          {"type": "doping", "dopant": "donor", "N0": "1e15[1/cm^3]", "selection": {"only": ["i"]}},
          {"type": "doping", "dopant": "donor", "N0": "1e19[1/cm^3]", "selection": {"only": ["n"]}},
          {"type": "srh", "selection": {"all": true}},
          {"type": "generation", "G": "G0", "selection": {"only": ["i"]}},
          {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["p"], "side": "left"} %1},
          {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["n"], "side": "right"}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%4"}}],
      "studies": [{"type": "study", "steps": [%2]}]})json")
        .arg(anode, steps, parameters, hmax)
        .toUtf8();
}

/// ∫ \a expression over the domains (times the thickness: the whole
/// device's).
double integral(const Solution& s, const QString& expression)
{
    Node in;
    in.type = QStringLiteral("integral");
    in.props.insert(QStringLiteral("expression"), expression);
    in.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}});
    const DerivedResult d = evaluateDerived(s, in);
    return d.values.isEmpty() ? NAN : d.values.front().value;
}

/// A lateral germanium p-i-n (0.7 µm between p⁺ and n⁺, 0.45 thick) on a
/// silicon slab waveguide in oxide, 15 µm long (the model's thickness), at
/// -1 V: its mode at 1550 nm fed P into germanium, its measured absorption
/// (no Franz-Keldysh: the light the same whatever the bias - a device
/// alone at another voltage the same light), the device cut into \a
/// slices; \a anode its contact's more, \a steps after the mode's (a
/// stationary step first).
QByteArray waveguidePd(int slices, const QString& power, const QString& anode, const QString& steps, bool coarse = false)
{
    const QString hmax = slowBuild() || coarse ? QStringLiteral("0.1") : QStringLiteral("0.05");
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "Va", "expression": "-1[V]"}, {"name": "P", "expression": "%2"}, {"name": "w", "expression": "0.7"}],
      "components": [{"unit": "um", "thickness": "15[um]",
        "geometry": [{"type": "rectangle", "label": "box", "position": ["-2", "-1"], "size": ["4", "1"]},
                     {"type": "rectangle", "label": "cladding", "position": ["-2", "0"], "size": ["4", "1.6"]},
                     {"type": "rectangle", "label": "slab", "position": ["-1.5", "0"], "size": ["3", "0.22"]},
                     {"type": "rectangle", "label": "pplus", "position": ["-w/2 - 0.2", "0.22"], "size": ["0.2", "0.45"]},
                     {"type": "rectangle", "label": "absorber", "position": ["-w/2", "0.22"], "size": ["w", "0.45"]},
                     {"type": "rectangle", "label": "nplus", "position": ["w/2", "0.22"], "size": ["0.2", "0.45"]},
                     {"type": "form union"}],
        "materials": [{"material": "Silicon dioxide", "selection": {"only": ["box", "cladding"]}},
                      {"material": "Silicon", "selection": {"only": ["slab"]}},
                      {"material": "Germanium on silicon", "selection": {"only": ["pplus", "absorber", "nplus"]}}],
        "physics": [{"type": "semiconductor", "tag": "semi", "selection": {"only": ["slab", "pplus", "absorber", "nplus"]}, "features": [
                       {"type": "model", "saturation": true, "vsatn": "6e6[cm/s]", "vsatp": "6e6[cm/s]", "selection": {"only": ["pplus", "absorber", "nplus"]}},
                       {"type": "doping", "dopant": "acceptor", "N0": "1e19[1/cm^3]", "selection": {"only": ["pplus"]}},
                       {"type": "doping", "dopant": "donor", "N0": "1e19[1/cm^3]", "selection": {"only": ["nplus"]}},
                       {"type": "doping", "dopant": "acceptor", "N0": "1e15[1/cm^3]", "selection": {"only": ["slab", "absorber"]}},
                       {"type": "srh", "selection": {"all": true}},
                       {"type": "optical", "mode": "emw", "P0": "P", "slices": %1, "selection": {"only": ["pplus", "absorber", "nplus"]}},
                       {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["pplus"], "side": "top"} %3},
                       {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["nplus"], "side": "top"}}]},
                    {"type": "modes", "tag": "emw", "features": [
                       {"type": "wave", "electroabsorption": false, "selection": {"only": ["pplus", "absorber", "nplus"]}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "0.25",
                 "features": [{"type": "size", "level": "domain", "selection": {"only": ["pplus", "absorber", "nplus", "slab"]}, "hmax": "%5"}]}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary", "physics": ["semi"]},
                                              {"type": "mode", "physics": ["emw"], "frequency": "1550[nm]", "modes": 1, "neff": "3.9"} %4]}]})json")
        .arg(QString::number(slices), power, anode, steps, hmax)
        .toUtf8();
}

/// The fundamental of \a y at \a f over its samples' last whole period
/// (times \a t, evenly spaced): its amplitude and its mean.
std::pair<double, double> fundamental(const std::vector<double>& t, const std::vector<double>& y, double f)
{
    const double T = 1 / f, end = t.back();
    std::complex<double> sum = 0;
    double mean = 0;
    int count = 0;
    for (std::size_t k = 0; k + 1 < t.size(); ++k) {
        if (t[k] < end - T - 1e-18) continue;
        sum += y[k] * std::exp(std::complex<double>(0, -2 * M_PI * f * t[k]));
        mean += y[k];
        ++count;
    }
    return {count > 0 ? 2 * std::abs(sum) / count : NAN, count > 0 ? mean / count : NAN};
}

} // namespace

class TestFemPhotodiodePower : public QObject
{
    Q_OBJECT

private slots:
    // Its anode behind a series resistance, the light making some 10 µA:
    // the contact's own voltage its source's less R I - the bias falling,
    // at 300 kΩ past nought -, and the current at it the device's own, held
    // there (the circuit's row and the device's curve met together).
    void aSeriesResistanceDropsTheBias()
    {
        const Model m = modelOf(pin(QStringLiteral(R"(, "circuit": "series", "Rs": "Rser", "RL": "0[Ohm]")"),
                                    QStringLiteral(R"({"type": "sweep", "parameters": [["Rser", "0, 5e4, 1e5, 3e5", "Ohm"]]}, {"type": "stationary"})"),
                                    QStringLiteral(R"(, {"name": "Rser", "expression": "0[Ohm]"})"))
                                    .replace("6e29[1/(m^3*s)]", "6e30[1/(m^3*s)]"));
        const Run r = run(m);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QVERIFY2(r.study.stopped.isEmpty(), qPrintable(r.study.stopped + QLatin1Char('\n') + r.study.log.join(QLatin1Char('\n'))));
        QCOMPARE(int(r.study.solutions.size()), 4);
        const double R[4] = {0, 5e4, 1e5, 3e5};
        double I0 = NAN;
        for (int k = 0; k < 4; ++k) {
            const Solution& s = *r.study.solutions[std::size_t(k)];
            const double V0 = global(s, QStringLiteral("semi.V_a")), Vc = global(s, QStringLiteral("semi.Vc_a")), I = global(s, QStringLiteral("semi.I_a"));
            if (k == 0) I0 = I;
            // Held at the voltage the circuit left it at: the same current.
            const Model held = modelOf(pin(QString(), QStringLiteral(R"({"type": "stationary"})"))
                                           .replace("6e29[1/(m^3*s)]", "6e30[1/(m^3*s)]")
                                           .replace(R"("expression": "-2[V]")", QStringLiteral(R"("expression": "%1[V]")").arg(Vc, 0, 'g', 17).toUtf8()));
            const Run h = run(held);
            QVERIFY2(h.error.isEmpty(), qPrintable(h.error));
            const double Ih = global(*h.study.solutions.back(), QStringLiteral("semi.I_a"));
            qInfo("R %6.0f Ω: V0 %.4f V, Vc %.6f V, I %.6g A (held there %.6g A, %.2e apart); V0 - Vc - R I %.2e V", R[k], V0, Vc, I, Ih,
                  std::abs(I / Ih - 1), V0 - Vc - R[k] * I);
            QVERIFY2(std::abs(V0 - Vc - R[k] * I) < 1e-9 * std::max(1.0, std::abs(R[k] * I)), qPrintable(QString::number(V0 - Vc - R[k] * I)));
            QVERIFY2(std::abs(I / Ih - 1) < 1e-6, qPrintable(QString::number(I / Ih - 1)));
            // (Its load none: no load current; the contact's own voltage the source's at 0 Ω.)
            QVERIFY(global(s, QStringLiteral("semi.IL_a")) == I);
            if (k == 0) QVERIFY(std::abs(Vc - V0) < 1e-12);
        }
        // The light's current out of the anode; at 300 kΩ the bias gone
        // past nought - forward: less of it collected.
        QVERIFY(I0 < -5e-6);
        const double V300 = global(*r.study.solutions.back(), QStringLiteral("semi.Vc_a"));
        QVERIFY2(V300 > -0.5, qPrintable(QString::number(V300)));
    }

    // Small-signal, the light modulated: through a series load of 50 Ω
    // (Rs nought, Cp none) the response semi.Hc as semi.HopRL's (the
    // device's admittance beside 50 Ω); a capacitance across it slows it.
    void theLoadInFrequency()
    {
        auto response = [](const QString& Cp, double* Hc, double* HopRL, double* Hop) {
            const Model m = modelOf(pin(QStringLiteral(R"(, "circuit": "series", "Rs": "0[Ohm]", "RL": "50[Ohm]", "Cp": "%1")").arg(Cp),
                                        QStringLiteral(R"({"type": "stationary"}, {"type": "frequency", "frequencies": "20[GHz]"})")));
            const Run r = run(m);
            if (!r.error.isEmpty()) return r.error;
            const Solution& s = *r.study.solutions.back();
            *Hc = global(s, QStringLiteral("semi.Hc_a"));
            *HopRL = global(s, QStringLiteral("semi.HopRL_a"));
            *Hop = global(s, QStringLiteral("semi.Hop_a"));
            return QString();
        };
        double hc = NAN, hrl = NAN, hop = NAN;
        QString why = response(QStringLiteral("0[F]"), &hc, &hrl, &hop);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        qInfo("at 20 GHz: Hop %.6f, through 50 Ω %.9f (HopRL %.9f)", hop, hc, hrl);
        QVERIFY2(std::abs(hc / hrl - 1) < 1e-9, qPrintable(QString::number(hc / hrl - 1)));
        double hcC = NAN;
        why = response(QStringLiteral("1[pF]"), &hcC, &hrl, &hop);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        // (1 pF across 50 Ω: 8 GHz.)
        const double rc = 1 / std::abs(std::complex<double>(1, 2 * M_PI * 20e9 * 50 * 1e-12));
        qInfo("with 1 pF across: %.6f (the RC alone %.6f of %.6f: %.6f)", hcC, rc, hc, rc * hc);
        QVERIFY2(std::abs(hcC / (rc * hc) - 1) < 0.01, qPrintable(QString::number(hcC / (rc * hc))));
    }

    // Through a bias tee into 100 kΩ, the light modulated by 5 % at 2 GHz
    // in time: the load carries the current's change alone (its mean
    // nought), as large as the small signal says through it.
    void aBiasTeeInTime()
    {
        if (slowBuild()) QSKIP("in time: Release builds only (minutes sanitized)");
        const bool slow = slowBuild();
        const double f = 2e9, depth = 0.05;
        const QString anode = QStringLiteral(R"(, "circuit": "biastee", "Rs": "0[Ohm]", "RL": "100[kOhm]")");
        const Run small = run(modelOf(pin(anode, QStringLiteral(R"({"type": "stationary"}, {"type": "frequency", "frequencies": "2[GHz]"})"))));
        QVERIFY2(small.error.isEmpty(), qPrintable(small.error));
        const Solution& ss = *small.study.solutions.back();
        const double Hc = global(ss, QStringLiteral("semi.Hc_a")), Gop = global(ss, QStringLiteral("semi.Gop"));
        // (At rest the tee's load carries nothing, the contact at its source.)
        const Run rest = run(modelOf(pin(anode, QStringLiteral(R"({"type": "stationary"})"))));
        QVERIFY2(rest.error.isEmpty(), qPrintable(rest.error));
        QCOMPARE(global(*rest.study.solutions.back(), QStringLiteral("semi.IL_a")), 0.0);
        QVERIFY(std::abs(global(*rest.study.solutions.back(), QStringLiteral("semi.Vc_a")) + 2) < 1e-9);
        const QString times = slow ? QStringLiteral("range(0, 12.5[ps], 1.5[ns])") : QStringLiteral("range(0, 6.25[ps], 2[ns])");
        QByteArray json = pin(anode, QStringLiteral(R"x({"type": "stationary"}, {"type": "transient", "times": "%1", "rtol": "%2"})x")
                                         .arg(times, slow ? QStringLiteral("1e-4") : QStringLiteral("1e-6")));
        json.replace(R"("G": "G0")", R"x("G": "G0*(1 + 0.05*sin(2*pi*2[GHz]*t))")x");
        const Run timed = run(modelOf(json));
        QVERIFY2(timed.error.isEmpty(), qPrintable(timed.error));
        Solution& s = *timed.study.solutions.back();
        std::vector<double> t, IL, Vc;
        for (int k = 0; k < s.snapshotCount(); ++k) {
            s.select(k);
            t.push_back(s.snapshotTime(k));
            IL.push_back(global(s, QStringLiteral("semi.IL_a")));
            Vc.push_back(global(s, QStringLiteral("semi.Vc_a")));
        }
        const auto [amplitude, mean] = fundamental(t, IL, f);
        // (The contact at its source on average: the tee's load carries
        // none of the current at rest - its 1 µA through 100 kΩ 0.1 V.)
        const double meanVc = fundamental(t, Vc, f).second, I0 = global(*rest.study.solutions.back(), QStringLiteral("semi.I_a"));
        qInfo("the contact's mean %.7f V (its source -2 V; RL I0 %.4f V)", meanVc, 1e5 * I0);
        QVERIFY2(std::abs(meanVc + 2) < 1e-3 * std::abs(1e5 * I0), qPrintable(QString::number(meanVc + 2)));
        const double expected = Hc * depth * Q * Gop;
        qInfo("through the tee into 100 kΩ at 2 GHz: |IL| %.6g A, the small signal's %.6g (%.4f), its mean %.3g A", amplitude, expected,
              amplitude / expected, mean);
        QVERIFY2(std::abs(amplitude / expected - 1) < (slow ? 0.03 : 0.01), qPrintable(QString::number(amplitude / expected)));
        QVERIFY2(std::abs(mean) < 0.02 * amplitude, qPrintable(QString::number(mean)));
    }

    // Dark, its source stepped from -1 to -1.1 V through 1 MΩ with 10 fF
    // across: the contact's voltage follows as a capacitor charges, its time
    // R (Cp + Cj) - Cj the junction's at -1.05 V, from its small signal.
    void aCapacitanceChargesThroughTheResistance()
    {
        if (slowBuild()) QSKIP("in time: Release builds only (minutes sanitized)");
        const QString dark = QStringLiteral("0[1/(m^3*s)]");
        QByteArray small = pin(QString(), QStringLiteral(R"({"type": "stationary"}, {"type": "frequency", "frequencies": "10[MHz]"})"));
        small.replace("6e29[1/(m^3*s)]", dark.toUtf8()).replace(R"("expression": "-2[V]")", R"("expression": "-1.05[V]")");
        const Run c = run(modelOf(small));
        QVERIFY2(c.error.isEmpty(), qPrintable(c.error));
        const double Cj = global(*c.study.solutions.back(), QStringLiteral("semi.Caa"));
        const double R = 1e6, Cp = 10e-15, tau = R * (Cp + Cj);
        QByteArray json = pin(QStringLiteral(R"(, "circuit": "series", "Rs": "1[MOhm]", "RL": "0[Ohm]", "Cp": "10[fF]")"),
                              QStringLiteral(R"x({"type": "stationary"}, {"type": "transient", "times": "range(0, 0.5[ns], 30[ns])", "rtol": "1e-6"})x"));
        json.replace("6e29[1/(m^3*s)]", dark.toUtf8()).replace(R"("V0": "Va")", R"x("V0": "-1[V] - 0.1[V]*(t > 0)")x");
        const Run timed = run(modelOf(json));
        QVERIFY2(timed.error.isEmpty(), qPrintable(timed.error));
        Solution& s = *timed.study.solutions.back();
        double worst = 0;
        QStringList said;
        for (int k = 1; k < s.snapshotCount(); ++k) {
            s.select(k);
            const double t = s.snapshotTime(k), Vc = global(s, QStringLiteral("semi.Vc_a"));
            const double expected = -1 - 0.1 * (1 - std::exp(-t / tau));
            worst = std::max(worst, std::abs(Vc - expected));
            if (k % 10 == 0) said << QStringLiteral("%1 ns %2 V (%3)").arg(t * 1e9).arg(Vc, 0, 'f', 6).arg(expected, 0, 'f', 6);
            // (Its source's less R times the series current: Cp's and the device's.)
            const double I = global(s, QStringLiteral("semi.IL_a")), V0 = global(s, QStringLiteral("semi.V_a"));
            QVERIFY2(std::abs(V0 - Vc - R * I) < 1e-9, qPrintable(QString::number(V0 - Vc - R * I)));
        }
        qInfo("Cj %.4g fF, τ %.4g ns; %s; worst %.2e V", Cj * 1e15, tau * 1e9, qPrintable(said.join(QLatin1String(", "))), worst);
        QVERIFY2(worst < 1e-3, qPrintable(QString::number(worst)));
    }

    // A Schottky diode (n-type silicon 1e16 cm⁻³, 2 µm), dark, its source
    // stepped -1 to -1.1 V through 1 MΩ: its contact's voltage follows as
    // its junction charges - on the metal, the barrier's depletion against
    // it - in R Cj, Cj its small signal's at -1.05 V; the source less R I
    // at each time.
    void aSchottkyChargesThroughTheResistance()
    {
        if (slowBuild()) QSKIP("in time: Release builds only (minutes sanitized)");
        auto diode = [](const QString& anode, const QString& steps) {
            const QString hmax = slowBuild() ? QStringLiteral("0.1") : QStringLiteral("0.05");
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um", "thickness": "10[um]",
                "geometry": [{"type": "rectangle", "label": "n", "position": ["0", "0"], "size": ["2", "0.5"]}],
                "materials": [{"material": "Silicon", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"all": true}},
                  {"type": "srh", "selection": {"all": true}},
                  {"type": "schottky", "name": "a", "phiB": "0.7[eV]", %1, "selection": {"of": ["n"], "side": "left"}},
                  {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["n"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "%3"}}],
              "studies": [{"type": "study", "steps": [%2]}]})json")
                .arg(anode, steps, hmax)
                .toUtf8();
        };
        const Run c = run(modelOf(diode(QStringLiteral(R"("V0": "-1.05[V]")"), QStringLiteral(R"({"type": "stationary"}, {"type": "frequency", "frequencies": "10[MHz]"})"))));
        QVERIFY2(c.error.isEmpty(), qPrintable(c.error));
        const double Cj = global(*c.study.solutions.back(), QStringLiteral("semi.Caa")), R = 1e6, tau = R * Cj;
        const Run timed = run(modelOf(diode(QStringLiteral(R"x("V0": "-1[V] - 0.1[V]*(t > 0)", "circuit": "series", "Rs": "1[MOhm]", "RL": "0[Ohm]")x"),
                                            QStringLiteral(R"x({"type": "stationary"}, {"type": "transient", "times": "range(0, 0.25[ns], 6[ns])", "rtol": "1e-6"})x"))));
        QVERIFY2(timed.error.isEmpty(), qPrintable(timed.error));
        Solution& s = *timed.study.solutions.back();
        double worst = 0, worstRow = 0;
        for (int k = 1; k < s.snapshotCount(); ++k) {
            s.select(k);
            const double t = s.snapshotTime(k), Vc = global(s, QStringLiteral("semi.Vc_a"));
            worst = std::max(worst, std::abs(Vc - (-1 - 0.1 * (1 - std::exp(-t / tau)))));
            worstRow = std::max(worstRow, std::abs(global(s, QStringLiteral("semi.V_a")) - Vc - R * global(s, QStringLiteral("semi.IL_a"))));
        }
        qInfo("a Schottky contact behind 1 MΩ: Cj %.4g fF, τ %.4g ns; its voltage the charge's to %.2e V; the source less R I to %.2e V", Cj * 1e15,
              tau * 1e9, worst, worstRow);
        QVERIFY2(Cj > 0 && worst < 2e-3, qPrintable(QString::number(worst)));
        QVERIFY2(worstRow < 1e-9, qPrintable(QString::number(worstRow)));
    }

    // Strong light (10 mA's worth) switched on through 100 kΩ, its source
    // -2 V - dark first, at its voltage: the contact's voltage past nought -
    // found from the dark solution by the walk's light and resistance (no
    // voltage to walk; all of it at once too far for Newton's method).
    void strongLightThroughALoad()
    {
        if (slowBuild()) QSKIP("strong light's walk: Release builds only (minutes sanitized)");
        const Run r = run(modelOf(pin(QStringLiteral(R"(, "circuit": "series", "Rs": "100[kOhm]", "RL": "0[Ohm]")"),
                                      QStringLiteral(R"x({"type": "sweep", "parameters": [["G0", "0, 6e33", "1/(m^3*s)"]]}, {"type": "stationary"})x"))));
        QVERIFY2(r.error.isEmpty() && r.study.stopped.isEmpty(), qPrintable(r.error + r.study.stopped));
        QCOMPARE(int(r.study.solutions.size()), 2);
        const Solution& s = *r.study.solutions.back();
        const double Vc = global(s, QStringLiteral("semi.Vc_a")), I = global(s, QStringLiteral("semi.I_a"));
        QStringList walk;
        bool fromRest = false;
        for (const QString& l : r.study.log) {
            if (l.contains(QLatin1String("reached"))) walk << l;
            fromRest = fromRest || l.contains(QLatin1String("solved from rest"));
        }
        // (From the dark solution - not from rest, again, as without the
        // walk's light it would be.)
        QVERIFY(!fromRest);
        qInfo("10 mA of light through 100 kΩ: Vc %.4f V, I %.5g A; %s", Vc, I, qPrintable(walk.join(QLatin1String("; "))));
        QVERIFY2(Vc > 0 && std::abs(-2 - Vc - 1e5 * I) < 1e-9 * std::max(1.0, std::abs(1e5 * I)), qPrintable(QStringLiteral("%1 %2").arg(Vc).arg(I)));
    }

    // Weak light (10 µW), the device in 4 slices each absorbing a quarter of
    // it - the first the shortest: their starts where e^(-2αz) has fallen by
    // a quarter of what the length takes, each carrying a quarter of the
    // current; together the current of the length's mean light, and its
    // small signal (its carriers too few to tell the slices apart).
    void slicesShareTheLight()
    {
        // (Sanitized: two slices, at rest alone - the small signal's, the
        // heat's, minutes more there.)
        const bool slow = slowBuild();
        const int S = slow ? 2 : 4;
        const QString steps = slow ? QStringLiteral(R"(, {"type": "stationary", "physics": ["semi"]})")
                                   : QStringLiteral(R"(, {"type": "stationary", "physics": ["semi"]}, {"type": "frequency", "physics": ["semi"], "frequencies": "20[GHz]"})");
        const Run one = run(modelOf(waveguidePd(1, QStringLiteral("10[uW]"), QString(), steps)));
        QVERIFY2(one.error.isEmpty(), qPrintable(one.error));
        const Run four = run(modelOf(waveguidePd(S, QStringLiteral("10[uW]"), QString(), steps)));
        QVERIFY2(four.error.isEmpty(), qPrintable(four.error));
        const Solution& s1 = *one.study.solutions.back();
        const Solution& s4 = *four.study.solutions.back();
        const double I1 = global(s1, QStringLiteral("semi.I_k")), I4 = global(s4, QStringLiteral("semi.I_k"));
        const double alpha = global(s4, QStringLiteral("emw.alpha")), L = 15e-6;
        QStringList said;
        double worstZ = 0, worstI = 0;
        for (int k = 1; k <= S; ++k) {
            const double z = global(s4, QStringLiteral("semi.zs%1").arg(k)), Is = global(s4, QStringLiteral("semi.Is%1_k").arg(k));
            const double expected = -std::log1p(-double(k - 1) / S * -std::expm1(-2 * alpha * L)) / (2 * alpha);
            worstZ = std::max(worstZ, std::abs(z - expected));
            worstI = std::max(worstI, std::abs(Is / (I4 / S) - 1));
            said << QStringLiteral("%1 µm %2 µA").arg(z * 1e6, 0, 'f', 3).arg(Is * 1e6, 0, 'f', 5);
        }
        qInfo("one slice %.6g A, %d slices %.6g A (%.2e apart): %s", I1, S, I4, I4 / I1 - 1, qPrintable(said.join(QLatin1String(", "))));
        QVERIFY2(worstZ < 1e-12, qPrintable(QString::number(worstZ)));
        QVERIFY2(worstI < 0.01, qPrintable(QString::number(worstI)));
        QVERIFY2(std::abs(I4 / I1 - 1) < 1e-3, qPrintable(QString::number(I4 / I1 - 1)));
        if (slow) return;
        // Small-signal: the light's response and the capacitance the
        // device's - each slice's by its share.
        const double H1 = global(s1, QStringLiteral("semi.Hop_k")), H4 = global(s4, QStringLiteral("semi.Hop_k"));
        const double C1 = global(s1, QStringLiteral("semi.Ckk")), C4 = global(s4, QStringLiteral("semi.Ckk"));
        qInfo("at 20 GHz Hop %.6f and %.6f; the capacitance %.6g F and %.6g F", H1, H4, C1, C4);
        QVERIFY2(std::abs(H4 / H1 - 1) < 3e-3, qPrintable(QString::number(H4 / H1 - 1)));
        QVERIFY2(C1 > 0 && std::abs(C4 / C1 - 1) < 1e-2, qPrintable(QStringLiteral("%1 %2").arg(C1).arg(C4)));
        // (The carriers' heat - q G (φp - φn) in it, the quasi-Fermi split
        // of each slice's own carriers: not the length's mean light's - the
        // slices' by their shares, each slice's as it is alone with its own
        // light; at rest: a frequency step's semiconductor is made anew, its
        // Joule heat none.)
        const QString rest = QStringLiteral(R"(, {"type": "stationary", "physics": ["semi"]})");
        const Run four0 = run(modelOf(waveguidePd(S, QStringLiteral("10[uW]"), QString(), rest)));
        QVERIFY2(four0.error.isEmpty(), qPrintable(four0.error));
        const double heat4 = integral(*four0.study.solutions.back(), QStringLiteral("semi.H"));
        double heat1 = 0;
        for (int k = 1; k <= S; ++k) {
            const double z0 = global(s4, QStringLiteral("semi.zs%1").arg(k)), z1 = k < S ? global(s4, QStringLiteral("semi.zs%1").arg(k + 1)) : L;
            auto mean = [alpha](double a, double b) { return (std::exp(-2 * alpha * a) - std::exp(-2 * alpha * b)) / (2 * alpha * (b - a)); };
            const Run a = run(modelOf(waveguidePd(1, QStringLiteral("%1[W]").arg(10e-6 * mean(z0, z1) / mean(0, L), 0, 'g', 17), QString(), rest)));
            QVERIFY2(a.error.isEmpty(), qPrintable(a.error));
            heat1 += integral(*a.study.solutions.back(), QStringLiteral("semi.H")) * (z1 - z0) / L;
        }
        qInfo("the carriers' heat, the slices alone %.6g W/m, together %.6g", heat1, heat4);
        QVERIFY2(heat1 > 0 && std::abs(heat4 / heat1 - 1) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(heat1).arg(heat4)));
    }

    // Strong light (5 mW) through 100 Ω: cut into 4 slices the first,
    // where the light is the strongest, collects the least of its pairs - its
    // carriers screening its field - and the device the less than with the
    // length's mean light; its response at 20 GHz the lower too.
    void theStartSaturatesFirst()
    {
        if (slowBuild()) QSKIP("strong light's walk: Release builds only");
        const QString anode = QStringLiteral(R"(, "circuit": "series", "Rs": "0[Ohm]", "RL": "100[Ohm]")");
        const QString steps = QStringLiteral(R"(, {"type": "stationary", "physics": ["semi"]}, {"type": "frequency", "physics": ["semi"], "frequencies": "20[GHz]"})");
        const Run one = run(modelOf(waveguidePd(1, QStringLiteral("5[mW]"), anode, steps)));
        QVERIFY2(one.error.isEmpty(), qPrintable(one.error));
        const Run four = run(modelOf(waveguidePd(4, QStringLiteral("5[mW]"), anode, steps)));
        QVERIFY2(four.error.isEmpty(), qPrintable(four.error));
        const Solution& s1 = *one.study.solutions.back();
        const Solution& s4 = *four.study.solutions.back();
        const double Iop = global(s4, QStringLiteral("semi.Iop"));
        const double I1 = global(s1, QStringLiteral("semi.I_k")), I4 = global(s4, QStringLiteral("semi.I_k"));
        QStringList said;
        std::vector<double> collected;
        for (int k = 1; k <= 4; ++k) {
            const double Is = global(s4, QStringLiteral("semi.Is%1_k").arg(k));
            collected.push_back(Is / (Iop / 4));
            said << QStringLiteral("%1").arg(collected.back(), 0, 'f', 4);
        }
        qInfo("5 mW through 100 Ω: one slice %.6g A (%.4f of q Gop), four %.6g A (%.4f) - each slice's share collected %s; Vc %.4f and %.4f V; "
              "Hc at 20 GHz %.4f and %.4f",
              I1, I1 / Iop, I4, I4 / Iop, qPrintable(said.join(QLatin1String(", "))), global(s1, QStringLiteral("semi.Vc_a")), global(s4, QStringLiteral("semi.Vc_a")),
              global(s1, QStringLiteral("semi.Hc_a")), global(s4, QStringLiteral("semi.Hc_a")));
        QVERIFY2(collected.front() < collected.back(), qPrintable(said.join(QLatin1String(", "))));
        QVERIFY2(std::abs(I4) < std::abs(I1), qPrintable(QStringLiteral("%1 %2").arg(I4).arg(I1)));
        // Slice 1 alone - a device of its light throughout, held at the
        // voltage the slices left the contact at: its current, by its share
        // of the length, slice 1's.
        const double alpha = global(s4, QStringLiteral("emw.alpha")), L = 15e-6, z2 = global(s4, QStringLiteral("semi.zs2"));
        auto mean = [alpha](double z0, double z1) { return (std::exp(-2 * alpha * z0) - std::exp(-2 * alpha * z1)) / (2 * alpha * (z1 - z0)); };
        const double factor = mean(0, z2) / mean(0, L), Vc = global(s4, QStringLiteral("semi.Vc_a"));
        QByteArray alone = waveguidePd(1, QStringLiteral("%1[W]").arg(5e-3 * factor, 0, 'g', 17), QString(), QStringLiteral(R"(, {"type": "stationary", "physics": ["semi"]})"));
        alone.replace(R"("expression": "-1[V]")", QStringLiteral(R"("expression": "%1[V]")").arg(Vc, 0, 'g', 17).toUtf8());
        const Run a = run(modelOf(alone));
        QVERIFY2(a.error.isEmpty(), qPrintable(a.error));
        const double Ialone = global(*a.study.solutions.back(), QStringLiteral("semi.I_k")) * z2 / L, Is1 = global(s4, QStringLiteral("semi.Is1_k"));
        qInfo("slice 1 (%.3f µm, %.2f times the mean light) alone %.8g A, in the device %.8g A (%.2e apart)", z2 * 1e6, factor, Ialone, Is1, Ialone / Is1 - 1);
        QVERIFY2(std::abs(Ialone / Is1 - 1) < 1e-5, qPrintable(QString::number(Ialone / Is1 - 1)));
    }

    // Two slices through a bias tee into 1 kΩ, the light (100 µW)
    // modulated by 5 % at 10 GHz in time - each slice its own history -: the
    // load's current as large as the sliced small signal says.
    void slicesThroughATeeInTime()
    {
        if (slowBuild()) QSKIP("in time: Release builds only (minutes sanitized)");
        const bool slow = slowBuild();
        const QString anode = QStringLiteral(R"(, "circuit": "biastee", "Rs": "0[Ohm]", "RL": "1[kOhm]")");
        // (Its mesh coarser: a transient of two slices.)
        const Run small = run(modelOf(waveguidePd(2, QStringLiteral("100[uW]"), anode,
                                                   QStringLiteral(R"(, {"type": "stationary", "physics": ["semi"]}, {"type": "frequency", "physics": ["semi"], "frequencies": "10[GHz]"})"), true)));
        QVERIFY2(small.error.isEmpty(), qPrintable(small.error));
        const Solution& ss = *small.study.solutions.back();
        const double Hc = global(ss, QStringLiteral("semi.Hc_a")), Gop = global(ss, QStringLiteral("semi.Gop"));
        QByteArray json = waveguidePd(2, QStringLiteral("100[uW]"), anode,
                                      QStringLiteral(R"x(, {"type": "stationary", "physics": ["semi"]}, {"type": "transient", "physics": ["semi"], "times": "%1", "rtol": "%2"})x")
                                          .arg(QStringLiteral("range(0, 5[ps], 300[ps])"), slow ? QStringLiteral("1e-4") : QStringLiteral("1e-5")),
                                      true);
        json.replace(R"("P0": "P")", R"x("P0": "P*(1 + 0.05*sin(2*pi*10[GHz]*t))")x");
        const Run timed = run(modelOf(json));
        QVERIFY2(timed.error.isEmpty(), qPrintable(timed.error));
        Solution& s = *timed.study.solutions.back();
        std::vector<double> t, IL;
        for (int k = 0; k < s.snapshotCount(); ++k) {
            s.select(k);
            t.push_back(s.snapshotTime(k));
            IL.push_back(global(s, QStringLiteral("semi.IL_a")));
        }
        const auto [amplitude, mean] = fundamental(t, IL, 10e9);
        const double expected = Hc * 0.05 * Q * Gop;
        qInfo("two slices through the tee at 10 GHz: |IL| %.6g A, the small signal's %.6g (%.4f), its mean %.3g A", amplitude, expected, amplitude / expected, mean);
        QVERIFY2(std::abs(amplitude / expected - 1) < (slow ? 0.03 : 0.01), qPrintable(QString::number(amplitude / expected)));
        QVERIFY2(std::abs(mean) < 0.02 * amplitude, qPrintable(QString::number(mean)));
    }

    // In time the slices step together: the light switched on over 20 ps,
    // the current at 300 ps the sliced stationary solution's.
    void slicesInTime()
    {
        const QString light = QStringLiteral("100[uW]");
        const Run rest = run(modelOf(waveguidePd(3, light, QString(), QStringLiteral(R"(, {"type": "stationary", "physics": ["semi"]})"))));
        QVERIFY2(rest.error.isEmpty(), qPrintable(rest.error));
        const double I = global(*rest.study.solutions.back(), QStringLiteral("semi.I_k"));
        QByteArray json = waveguidePd(3, light, QString(), QStringLiteral(R"x(, {"type": "transient", "physics": ["semi"], "times": "range(0, 25[ps], 300[ps])", "rtol": "1e-5"})x"));
        json.replace(R"("P0": "P")", R"x("P0": "P*min(t/20[ps], 1)")x");
        const Run timed = run(modelOf(json));
        QVERIFY2(timed.error.isEmpty(), qPrintable(timed.error));
        Solution& s = *timed.study.solutions.back();
        s.select(s.snapshotCount() - 1);
        const double It = global(s, QStringLiteral("semi.I_k"));
        double sum = 0;
        for (int k = 1; k <= 3; ++k) sum += global(s, QStringLiteral("semi.Is%1_k").arg(k));
        qInfo("three slices at 300 ps %.6g A (at rest %.6g, %.2e apart; the slices' sum %.6g)", It, I, It / I - 1, sum);
        QVERIFY2(std::abs(It / I - 1) < 1e-3, qPrintable(QString::number(It / I - 1)));
        QVERIFY2(std::abs(sum / It - 1) < 1e-12, qPrintable(QString::number(sum / It - 1)));
    }
};

QTEST_GUILESS_MAIN(TestFemPhotodiodePower)
#include "test_fem_photodiode_power.moc"
