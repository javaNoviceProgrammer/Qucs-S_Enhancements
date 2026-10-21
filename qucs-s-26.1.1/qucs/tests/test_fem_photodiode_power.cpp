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

/// ∫ \a expression over the domains' cross-section: per metre of the
/// length (the currents are the thickness's).
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

/// \a expression at (\a x, \a y) (the model's µm).
double at(const Solution& s, const QString& expression, double x, double y)
{
    const Locator locator(s.mesh());
    const std::optional<double> v = evaluateAt(s, locator, expression, QPointF(x, y));
    return v ? *v : NAN;
}

/// The p-i-n of pin() dark at \a Va, its physics at \a T, its lattice at
/// \a TL (empty: the physics' T); \a features more.
QByteArray darkPin(const QString& Va, const QString& T, const QString& TL, const QString& features = QString())
{
    QByteArray j = pin(QString(), QStringLiteral(R"({"type": "stationary"})"));
    j.replace("6e29[1/(m^3*s)]", "0[1/(m^3*s)]");
    j.replace(R"("expression": "-2[V]")", QStringLiteral(R"("expression": "%1")").arg(Va).toUtf8());
    j.replace(R"("tag": "semi", "features": [)",
              QStringLiteral(R"("tag": "semi", "T": "%1", "TL": "%2", "features": [%3)").arg(T, TL, features).toUtf8());
    return j;
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
        // (A Harmonics node's H1 and mean the same, its trapezoids over the
        // last period's evenly spaced times the sum's.)
        Node harm;
        harm.type = QStringLiteral("harmonics");
        harm.props.insert(QStringLiteral("expression"), QStringLiteral("semi.IL_a"));
        harm.props.insert(QStringLiteral("f0"), QStringLiteral("2[GHz]"));
        const DerivedResult d = evaluateDerived(s, harm);
        QVERIFY2(d.error.isEmpty() && d.values.size() >= 2, qPrintable(d.error));
        QVERIFY2(std::abs(d.values[1].value / amplitude - 1) < 1e-9, qPrintable(QStringLiteral("%1 %2").arg(d.values[1].value).arg(amplitude)));
        QVERIFY2(std::abs(d.values[0].value - mean) < 1e-9 * amplitude, qPrintable(QStringLiteral("%1 %2").arg(d.values[0].value).arg(mean)));
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
        if (slowBuild()) QSKIP("in time: Release builds only (minutes sanitized)");
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

    // The light's heat in semi.H: the device's ∫ semi.H its contacts' I V
    // and the power its pairs are made with, semi.Pabs, together - lit
    // reversed and short-circuited, a User-Defined Generation's pairs at
    // the gap (silicon's 1.12 eV) or at a photon energy given, a mode's at
    // ħω each (germanium on silicon: a heterojunction's, saturated); and
    // sliced. (Dark, the germanium's ∫ semi.H was 3 % off I V before: its
    // recombination heat by the recombining parts' volume, not the
    // corners' the integral takes.)
    void theLightsHeat()
    {
        const Run gap = run(modelOf(pin(QString(), QStringLiteral(R"({"type": "sweep", "parameters": [["Va", "-2, 0", "V"]]}, {"type": "stationary"})"))));
        QVERIFY2(gap.error.isEmpty(), qPrintable(gap.error));
        QCOMPARE(int(gap.study.solutions.size()), 2);
        const double Va[2] = {-2, 0};
        for (int k = 0; k < 2; ++k) {
            const Solution& s = *gap.study.solutions[std::size_t(k)];
            const double I = global(s, QStringLiteral("semi.I_a")), P = global(s, QStringLiteral("semi.Pabs")), G = global(s, QStringLiteral("semi.Gop"));
            const double H = integral(s, QStringLiteral("semi.H")) * 10e-6, light = integral(s, QStringLiteral("semi.Hlight")) * 10e-6;
            qInfo("at %g V: ∫ semi.H %.9g W, I V %.6g W, Pabs %.9g W (%.6f eV a pair), the light's %.6g W", Va[k], H, I * Va[k], P, P / (Q * G), light);
            QVERIFY2(P > 0 && std::abs(P / (Q * G) - 1.12) < 0.01, qPrintable(QString::number(P / (Q * G))));
            QVERIFY2(std::abs(H - (I * Va[k] + P)) < 1e-9 * (std::abs(I * Va[k]) + P), qPrintable(QStringLiteral("%1 %2").arg(H).arg(I * Va[k] + P)));
            QVERIFY2(light > 0 && light < P, qPrintable(QStringLiteral("%1 %2").arg(light).arg(P)));
        }
        QByteArray json = pin(QString(), QStringLiteral(R"({"type": "stationary"})"));
        json.replace(R"("G": "G0",)", R"("G": "G0", "Eph": "2.5[eV]",)");
        const Run photon = run(modelOf(json));
        QVERIFY2(photon.error.isEmpty(), qPrintable(photon.error));
        {
            const Solution& s = *photon.study.solutions.back();
            const double I = global(s, QStringLiteral("semi.I_a")), P = global(s, QStringLiteral("semi.Pabs")), G = global(s, QStringLiteral("semi.Gop"));
            const double H = integral(s, QStringLiteral("semi.H")) * 10e-6;
            qInfo("2.5 eV photons: ∫ semi.H %.9g W, I V %.6g W, Pabs %.9g W", H, -2 * I, P);
            QVERIFY2(std::abs(P / (Q * G * 2.5) - 1) < 1e-12, qPrintable(QString::number(P / (Q * G * 2.5) - 1)));
            QVERIFY2(std::abs(H - (-2 * I + P)) < 1e-9 * (std::abs(2 * I) + P), qPrintable(QStringLiteral("%1 %2").arg(H).arg(-2 * I + P)));
        }
        const QString rest = QStringLiteral(R"(, {"type": "stationary", "physics": ["semi"]})");
        // (η 0.8 too: the photons absorbed that make no pair heat as well.
        // Sanitized, that one alone: each some 50 s there.)
        std::vector<std::pair<int, double>> cases = {{1, 0.8}};
        if (!slowBuild()) cases = {{1, 1.0}, {2, 1.0}, {1, 0.8}};
        for (const auto& [slices, eta] : cases) {
            QByteArray json = waveguidePd(slices, QStringLiteral("1[mW]"), QString(), rest, true);
            if (eta != 1) json.replace(R"("type": "optical", "mode": "emw",)", R"("type": "optical", "mode": "emw", "eta": "0.8",)");
            const Run r = run(modelOf(json));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.study.solutions.back();
            const double I = global(s, QStringLiteral("semi.I_a")), P = global(s, QStringLiteral("semi.Pabs")), G = global(s, QStringLiteral("semi.Gop"));
            const double H = integral(s, QStringLiteral("semi.H")) * 15e-6, photon = 6.62607015e-34 * 299792458.0 / 1550e-9;
            qInfo("%d slice(s), η %g, 1 mW at 1550 nm: ∫ semi.H %.9g W, I V %.9g W, Pabs %.9g W (%.6g of a photon a pair; %.2e apart)", slices, eta,
                  H, -I, P, P / (G * photon), H / (-I + P) - 1);
            QVERIFY2(std::abs(P / (G * photon) - 1 / eta) < 1e-12, qPrintable(QString::number(P / (G * photon) - 1 / eta)));
            // (Its mobilities held where saturated: Newton's tolerance, 2e-9.)
            QVERIFY2(std::abs(H - (-I + P)) < 1e-8 * (std::abs(I) + P), qPrintable(QStringLiteral("%1 %2").arg(H).arg(-I + P)));
        }
        // Dark, saturated: ∫ semi.H I V. (Each triangle's share of an edge's
        // Joule heat over the edge's conductance with its mobilities held,
        // not their own sum: 2e-6 off.)
        const Run dark = run(modelOf(waveguidePd(1, QStringLiteral("0[W]"), QString(), rest, true)));
        QVERIFY2(dark.error.isEmpty(), qPrintable(dark.error));
        const double Id = global(*dark.study.solutions.back(), QStringLiteral("semi.I_a"));
        const double Hd = integral(*dark.study.solutions.back(), QStringLiteral("semi.H")) * 15e-6;
        qInfo("dark: ∫ semi.H %.9g W, I V %.9g W (%.2e apart)", Hd, -Id, Hd / -Id - 1);
        QVERIFY2(std::abs(Hd / -Id - 1) < 1e-9, qPrintable(QStringLiteral("%1 %2").arg(Hd).arg(-Id)));
    }

    // A lattice temperature TL in the bands: the p-i-n dark, reversed 5 V
    // (its i layer depleted), its physics at 300 K and its lattice at 350 K
    // against the physics at 350 K - its intrinsic density and its gap
    // shown (Varshni's, silicon's) the same; its current - the i layer's
    // thermal generation, q ∫ ni/(τn + τp) - 35 times the current at 300 K,
    // as ni, and the same to 2 %: where n or p is near ni at the layer's
    // edges is of the carriers' kT, the physics'.
    void theLatticeInTheBands()
    {
        const Run cold = run(modelOf(darkPin(QStringLiteral("-5[V]"), QStringLiteral("300[K]"), QString())));
        const Run lattice = run(modelOf(darkPin(QStringLiteral("-5[V]"), QStringLiteral("300[K]"), QStringLiteral("350[K]"))));
        const Run warm = run(modelOf(darkPin(QStringLiteral("-5[V]"), QStringLiteral("350[K]"), QString())));
        for (const Run* r : {&cold, &lattice, &warm}) QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
        const Solution &c = *cold.study.solutions.back(), &l = *lattice.study.solutions.back(), &w = *warm.study.solutions.back();
        const double Ic = global(c, QStringLiteral("semi.I_k")), Il = global(l, QStringLiteral("semi.I_k")), Iw = global(w, QStringLiteral("semi.I_k"));
        const double nil = at(l, QStringLiteral("semi.ni"), 1.3, 0.25), niw = at(w, QStringLiteral("semi.ni"), 1.3, 0.25), nic = at(c, QStringLiteral("semi.ni"), 1.3, 0.25);
        const QString gap = QStringLiteral("semi.Ec - semi.Ev");
        const double Egl = at(l, gap, 1.3, 0.25), Egw = at(w, gap, 1.3, 0.25), Egc = at(c, gap, 1.3, 0.25);
        qInfo("dark at -5 V: %.6g A at 300 K, %.6g A its lattice at 350 K, %.6g A at 350 K (%.2e apart); ni %.6g, %.6g, %.6g /m³; "
              "the gap %.6f, %.6f, %.6f eV",
              Ic, Il, Iw, Il / Iw - 1, nic, nil, niw, Egc / Q, Egl / Q, Egw / Q);
        QVERIFY2(std::abs(nil / niw - 1) < 1e-9, qPrintable(QStringLiteral("%1 %2").arg(nil).arg(niw)));
        QVERIFY2(std::abs(Egl - Egw) < 1e-9 * Q, qPrintable(QStringLiteral("%1 %2").arg(Egl / Q).arg(Egw / Q)));
        // (Silicon's Varshni: 4.73e-4 eV/K, 636 K.)
        const double varshni = 1.12 - 4.73e-4 * (350.0 * 350 / 986 - 300.0 * 300 / 936);
        QVERIFY2(std::abs(Egl / Q - varshni) < 1e-9, qPrintable(QStringLiteral("%1 %2").arg(Egl / Q, 0, 'g', 12).arg(varshni, 0, 'g', 12)));
        QVERIFY2(std::abs(Il / Iw - 1) < 0.03, qPrintable(QStringLiteral("%1 %2").arg(Il).arg(Iw)));
        QVERIFY2(Il / Ic > 10, qPrintable(QStringLiteral("%1 %2").arg(Il).arg(Ic)));
    }

    // Saturated at its lattice's temperature: a bar of n-type silicon
    // (1e17 cm⁻³, 20 µm, 1 µm high, a metre thick) at 20 V, its electrons'
    // velocity µE/√(1 + (µE/vsat)²), µ 0.14 m²/(V s) (TL/300 K)^-2.5, vsat
    // Quay's 1e5 m/s /(0.74 + 0.26 TL/300 K) - its lattice at 400 K
    // (the physics at 300 K) and the physics at 400 K, to 1e-4; at 300 K.
    void saturationOfTheLattice()
    {
        auto bar = [](const QString& T, const QString& TL) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "label": "bar", "size": ["20", "1"]}],
                "materials": [{"material": "Silicon", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "T": "%1", "TL": "%2", "features": [
                  {"type": "model", "saturation": true, "driving": "field", "selection": {"all": true}},
                  {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": true}},
                  {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}},
                  {"type": "ohmic", "name": "2", "V0": "20[V]", "selection": {"of": ["bar"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.5"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                .arg(T, TL)
                .toUtf8();
        };
        auto expected = [](double T) {
            const double mu = 0.14 * std::pow(T / 300, -2.5), vsat = 1e5 / (0.74 + 0.26 * T / 300), E = 20 / 20e-6;
            return Q * 1e23 * mu * E / std::sqrt(1 + std::pow(mu * E / vsat, 2)) * 1e-6;
        };
        const double I300 = expected(300), I400 = expected(400);
        const Run cold = run(modelOf(bar(QStringLiteral("300[K]"), QString())));
        const Run lattice = run(modelOf(bar(QStringLiteral("300[K]"), QStringLiteral("400[K]"))));
        const Run warm = run(modelOf(bar(QStringLiteral("400[K]"), QString())));
        for (const Run* r : {&cold, &lattice, &warm}) QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
        const double Ic = global(*cold.study.solutions.back(), QStringLiteral("semi.I_2"));
        const double Il = global(*lattice.study.solutions.back(), QStringLiteral("semi.I_2"));
        const double Iw = global(*warm.study.solutions.back(), QStringLiteral("semi.I_2"));
        qInfo("saturated: %.6g A at 300 K (%.6g); its lattice at 400 K %.6g A, the physics at 400 K %.6g A (%.6g)", Ic, I300, Il, Iw, I400);
        QVERIFY2(std::abs(Ic / I300 - 1) < 1e-4, qPrintable(QStringLiteral("%1 %2").arg(Ic).arg(I300)));
        QVERIFY2(std::abs(Il / I400 - 1) < 1e-4, qPrintable(QStringLiteral("%1 %2").arg(Il).arg(I400)));
        QVERIFY2(std::abs(Iw / I400 - 1) < 1e-4, qPrintable(QStringLiteral("%1 %2").arg(Iw).arg(I400)));
    }

    // Impact ionization at its lattice's temperature: the p-i-n dark at
    // -25 V (its i layer's field some 1.5e5 V/cm), the ionization integrals
    // alone - silicon's van Overstraeten and de Man, γ of 63 meV: its
    // lattice at 400 K the same as its coefficients times γ(400 K) =
    // tanh(ħω/2k 300 K)/tanh(ħω/2k 400 K) given at 300 K, to 1e-9 (the
    // same field); the physics at 400 K likewise; a sixth fewer pairs a
    // micrometre than at 300 K. (Its lattice and the physics at 400 K
    // apart: the built-in potential's kT, the physics', its field 1.7 %
    // weaker - α ten times as steep.)
    void ionizationOfTheLattice()
    {
        const double phonon = 0.063 * Q, kB = 1.380649e-23;
        const double g = std::tanh(phonon / (2 * kB * 300)) / std::tanh(phonon / (2 * kB * 400));
        const QString silicon = QStringLiteral(R"({"type": "impact", "model": "silicon", "currents": false, "selection": {"all": true}},)");
        const QString scaled = QStringLiteral(R"({"type": "impact", "model": "user", "currents": false, "Eph": "0[eV]",
            "an": "%1[1/cm]", "bn": "%2[V/cm]", "an2": "%1[1/cm]", "bn2": "%2[V/cm]", "ap": "%3[1/cm]", "bp": "%4[V/cm]",
            "ap2": "%5[1/cm]", "bp2": "%6[V/cm]", "E0": "4e5[V/cm]", "betan": "1", "betap": "1", "selection": {"all": true}},)")
                                   .arg(7.03e5 * g, 0, 'g', 17)
                                   .arg(1.231e6 * g, 0, 'g', 17)
                                   .arg(1.582e6 * g, 0, 'g', 17)
                                   .arg(2.036e6 * g, 0, 'g', 17)
                                   .arg(6.71e5 * g, 0, 'g', 17)
                                   .arg(1.693e6 * g, 0, 'g', 17);
        const QString V = QStringLiteral("-25[V]"), T300 = QStringLiteral("300[K]"), T400 = QStringLiteral("400[K]");
        // (Sanitized, not against 300 K: each run some 22 s there.)
        const Run cold = slowBuild() ? Run() : run(modelOf(darkPin(V, T300, QString(), silicon)));
        const Run lattice = run(modelOf(darkPin(V, T300, T400, silicon)));
        const Run latticeGiven = run(modelOf(darkPin(V, T300, T400, scaled)));
        const Run warm = run(modelOf(darkPin(V, T400, QString(), silicon)));
        const Run warmGiven = run(modelOf(darkPin(V, T400, QString(), scaled)));
        for (const Run* r : {&cold, &lattice, &latticeGiven, &warm, &warmGiven}) QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
        auto of = [](const Run& r, const char* name) { return r.study.solutions.empty() ? NAN : global(*r.study.solutions.back(), QString::fromLatin1(name)); };
        qInfo("ionization integrals at -25 V, γ(400 K) %.6f: In %.6g at 300 K; its lattice at 400 K %.9g (given %.9g); the physics at "
              "400 K %.9g (given %.9g); Ip %.6g, %.9g (%.9g), %.9g (%.9g)",
              g, of(cold, "semi.In"), of(lattice, "semi.In"), of(latticeGiven, "semi.In"), of(warm, "semi.In"), of(warmGiven, "semi.In"),
              of(cold, "semi.Ip"), of(lattice, "semi.Ip"), of(latticeGiven, "semi.Ip"), of(warm, "semi.Ip"), of(warmGiven, "semi.Ip"));
        for (const char* name : {"semi.In", "semi.Ip"}) {
            QVERIFY2(std::abs(of(lattice, name) / of(latticeGiven, name) - 1) < 1e-9,
                     qPrintable(QStringLiteral("%1 %2 %3").arg(QLatin1String(name)).arg(of(lattice, name)).arg(of(latticeGiven, name))));
            QVERIFY2(std::abs(of(warm, name) / of(warmGiven, name) - 1) < 1e-9,
                     qPrintable(QStringLiteral("%1 %2 %3").arg(QLatin1String(name)).arg(of(warm, name)).arg(of(warmGiven, name))));
        }
        if (!slowBuild())
            QVERIFY2(of(lattice, "semi.In") < 0.5 * of(cold, "semi.In"), qPrintable(QStringLiteral("%1 %2").arg(of(lattice, "semi.In")).arg(of(cold, "semi.In"))));
    }

    // The direct gap's absorption at the absorber's temperature: the
    // germanium photodiode's mode at 1550 nm, Franz-Keldysh's on in the
    // device's own field, its absorber at 350 K - its gap 0.773 eV less
    // Varshni's 5.82e-4 eV/K (T²/(T + 296 K) - ...) - the same as the gap
    // so lowered given, to 1e-9; more than at 300 K (the photon above the
    // edge by 27 meV, then by 49).
    void theEdgeOfTheLattice()
    {
        auto lit = [](const QString& wave) {
            QByteArray j = waveguidePd(1, QStringLiteral("1[uW]"), QString(), QString(), true);
            j.replace(R"({"type": "wave", "electroabsorption": false,)", QStringLiteral(R"({"type": "wave", "electroabsorption": true, %1)").arg(wave).toUtf8());
            return j;
        };
        const double shift = 5.82e-4 * (350.0 * 350 / 646 - 300.0 * 300 / 596);
        const Run warm = run(modelOf(lit(QStringLiteral(R"("Tea": "350[K]",)"))));
        const Run given = run(modelOf(lit(QStringLiteral(R"("easource": "user", "Egd": "%1[eV]", "mr": "0.03", "Afk": "2e4[1/cm]",)").arg(0.773 - shift, 0, 'g', 17))));
        for (const Run* r : {&warm, &given}) QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
        const double aw = global(*warm.study.solutions.back(), QStringLiteral("emw.alpha"));
        const double ag = global(*given.study.solutions.back(), QStringLiteral("emw.alpha"));
        qInfo("the mode's α at 1550 nm: its absorber at 350 K %.9g /m (the gap %.6f eV given: %.9g)", aw, 0.773 - shift, ag);
        QVERIFY2(std::abs(aw / ag - 1) < 1e-9, qPrintable(QStringLiteral("%1 %2").arg(aw).arg(ag)));
        // (Sanitized, not against 300 K: each run some 47 s there.)
        if (slowBuild()) return;
        const Run cold = run(modelOf(lit(QString())));
        QVERIFY2(cold.error.isEmpty(), qPrintable(cold.error));
        const double ac = global(*cold.study.solutions.back(), QStringLiteral("emw.alpha"));
        qInfo("at 300 K %.9g /m", ac);
        QVERIFY2(aw > 1.1 * ac, qPrintable(QStringLiteral("%1 %2").arg(aw).arg(ac)));
    }

    // The whole loop: the p-i-n under strong light (3 mA's worth, its
    // photons 1.5 eV) reversed 5 V, a heat physics its source semi.H, its
    // ends at 300 K, the semiconductor's lattice at its temperature (TL =
    // ht.T) - solved in turn to rest: ∫ semi.H its I V and Pabs together;
    // its middle some 10 K warmer.
    void aLitDiodeWarmsItself()
    {
        if (slowBuild()) QSKIP("self-heating's turns: Release builds only");
        QByteArray j = pin(QString(), QStringLiteral(R"({"type": "stationary"})"));
        j.replace("6e29[1/(m^3*s)]", "2e33[1/(m^3*s)]");
        j.replace(R"("expression": "-2[V]")", R"("expression": "-5[V]")");
        j.replace(R"("G": "G0",)", R"("G": "G0", "Eph": "1.5[eV]",)");
        j.replace(R"("tag": "semi", "features": [)", R"("tag": "semi", "TL": "ht.T", "features": [)");
        j.replace(R"({"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["n"], "side": "right"}}]})",
                  R"({"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["n"], "side": "right"}}]},
            {"type": "heat", "tag": "ht", "features": [
              {"type": "source", "Q0": "semi.H", "selection": {"all": true}},
              {"type": "init", "T0": "300[K]", "selection": {"all": true}},
              {"type": "temperature", "T0": "300[K]", "selection": {"of": ["p"], "side": "left"}},
              {"type": "temperature", "T0": "300[K]", "selection": {"of": ["n"], "side": "right"}}]})");
        const Run r = run(modelOf(j));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.study.solutions.back();
        const double I = global(s, QStringLiteral("semi.I_a")), P = global(s, QStringLiteral("semi.Pabs"));
        const double H = integral(s, QStringLiteral("semi.H")) * 10e-6;
        const double Tmid = at(s, QStringLiteral("ht.T"), 1.3, 0.25);
        qInfo("lit and warm: %.6g A at -5 V, ∫ semi.H %.9g W (I V %.6g W, Pabs %.6g W); %.3f K at its middle", I, H, -5 * I, P, Tmid);
        QVERIFY2(std::abs(H - (-5 * I + P)) < 1e-6 * (std::abs(5 * I) + P), qPrintable(QStringLiteral("%1 %2").arg(H).arg(-5 * I + P)));
        QVERIFY2(Tmid > 303, qPrintable(QString::number(Tmid)));
    }

    // The light and the carriers in turn: germanium on silicon,
    // Franz-Keldysh's in its own field, 5 mW through 100 Ω - its bias
    // falling from -1 V to -0.4, its field with it. One pass absorbs in the
    // dark field; in turn, in the lit one: its mode's α 6 % less (the photon
    // above the edge, a weaker field's oscillation), settled to 1e-6 in
    // three passes, one pass more the same to 1e-5. (Its 15 µm absorb the
    // light nearly whole either way: its pairs 0.09 % fewer.)
    void theLightInTurn()
    {
        if (slowBuild()) QSKIP("strong light's passes: Release builds only (each some 50 s sanitized)");
        const QString anode = QStringLiteral(R"(, "circuit": "series", "Rs": "100[Ohm]", "RL": "0[Ohm]")");
        auto lit = [&anode](const QString& steps) {
            QByteArray j = waveguidePd(1, QStringLiteral("5[mW]"), anode, steps, true);
            j.replace(R"({"type": "wave", "electroabsorption": false,)", R"({"type": "wave", "electroabsorption": true,)");
            return j;
        };
        const QString once = QStringLiteral(R"(, {"type": "stationary", "physics": ["semi"]})");
        const QString turns = QStringLiteral(R"(, {"type": "stationary", "physics": ["semi"], "lightloop": true, "lighttol": "1e-6"})");
        const QString more = turns + QStringLiteral(R"(, {"type": "mode", "physics": ["emw"], "frequency": "1550[nm]", "modes": 1, "neff": "3.9"},
            {"type": "stationary", "physics": ["semi"]})");
        const Run a = run(modelOf(lit(once))), b = run(modelOf(lit(turns))), c = run(modelOf(lit(more)));
        for (const Run* r : {&a, &b, &c}) QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
        auto of = [](const Run& r, const char* name) { return global(*r.study.solutions.back(), QString::fromLatin1(name)); };
        QString said;
        for (const QString& line : b.study.solutions.back()->log)
            if (line.contains(QLatin1String("in turn"))) said = line;
        qInfo("5 mW through 100 Ω: one pass Gop %.9g /s, α %.6g /m, Vc %.6f V; in turn %.9g /s, α %.6g /m, Vc %.6f V (%s); a pass more %.9g /s",
              of(a, "semi.Gop"), of(a, "emw.alpha"), of(a, "semi.Vc_a"), of(b, "semi.Gop"), of(b, "emw.alpha"), of(b, "semi.Vc_a"), qPrintable(said),
              of(c, "semi.Gop"));
        QVERIFY2(!said.isEmpty(), "no passes said");
        QVERIFY2(std::abs(of(c, "semi.Gop") / of(b, "semi.Gop") - 1) < 1e-5, qPrintable(QStringLiteral("%1 %2").arg(of(c, "semi.Gop")).arg(of(b, "semi.Gop"))));
        QVERIFY2(of(b, "emw.alpha") < 0.95 * of(a, "emw.alpha"), qPrintable(QStringLiteral("%1 %2").arg(of(b, "emw.alpha")).arg(of(a, "emw.alpha"))));
    }

    // In turn with no mode analysis before it, or no light: said.
    void theLightInTurnNeedsALight()
    {
        const Run none = run(modelOf(pin(QString(), QStringLiteral(R"({"type": "stationary", "lightloop": true})"))));
        QVERIFY2(none.error.contains(QLatin1String("no Mode Analysis step before it")), qPrintable(none.error));
        QByteArray j = waveguidePd(1, QStringLiteral("1[uW]"), QString(), QStringLiteral(R"(, {"type": "stationary", "physics": ["semi"], "lightloop": true})"), true);
        j.replace(R"({"type": "optical", "mode": "emw", "P0": "P", "slices": 1, "selection": {"only": ["pplus", "absorber", "nplus"]}},)", "");
        const Run dark = run(modelOf(j));
        QVERIFY2(dark.error.contains(QLatin1String("no semiconductor is lit")), qPrintable(dark.error));
    }

    // Thermionic emission over a heterojunction: n-GaAs | n-Al0.3Ga0.7As
    // (1e18 cm⁻³ each, 40 nm - the barrier's depletion some 17 nm -, 5 nm
    // high), 1 mV across. Emitted over its step the interface adds Vt/(q
    // v_R n' A) to the abrupt drift and diffusion's resistance, to 2 % - n'
    // the barrier's electrons at it, v_R = √(kT/2π m*), m* of its Nc (read
    // back: n e^((Ec - Efn)/kT)) -, the same to 1 % as its mesh is halved
    // (0.183, 0.170, 0.166, 0.164 Ω at 2, 1, 0.5 and 0.25 nm, the first
    // coarser than its Debye length); tunnelling through its spike divides
    // that by 1 + δ, δ of the field beside it (15 at 2.2e7 V/m), to 1 %.
    void emittedOverAHeterojunction()
    {
        auto junction = [](const QString& kind, const QString& hmax) {
            return modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "nm",
                "geometry": [{"type": "rectangle", "label": "a", "size": ["40", "5"]},
                             {"type": "rectangle", "label": "b", "position": ["40", "0"], "size": ["40", "5"]}],
                "materials": [{"material": "Gallium arsenide", "selection": {"only": ["a"]}},
                              {"material": "Aluminium gallium arsenide", "x": "0.3", "selection": {"only": ["b"]}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "heterojunctions": "%1", "features": [
                  {"type": "doping", "dopant": "donor", "N0": "1e18[1/cm^3]", "selection": {"all": true}},
                  {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["a"], "side": "left"}},
                  {"type": "ohmic", "name": "2", "V0": "1[mV]", "selection": {"of": ["b"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "%2"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                               .arg(kind, hmax)
                               .toUtf8());
        };
        // (Sanitized, 0.5 and 1 nm: 2.6 % apart there, the finer pair's
        // runs past QtTest's 300 s.)
        const bool slow = slowBuild();
        const QString fine = slow ? QStringLiteral("0.5") : QStringLiteral("0.25"), coarse = slow ? QStringLiteral("1") : QStringLiteral("0.5");
        const double within = slow ? 0.03 : 0.01;
        const Run dd = run(junction(QStringLiteral("abrupt"), fine)), te = run(junction(QStringLiteral("thermionic"), fine));
        const Run teCoarse = run(junction(QStringLiteral("thermionic"), coarse)), tun = run(junction(QStringLiteral("tunnelling"), fine));
        for (const Run* r : {&dd, &te, &teCoarse, &tun}) QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
        const Solution &sd = *dd.study.solutions.back(), &st = *te.study.solutions.back(), &su = *tun.study.solutions.back();
        const double V = 1e-3, A = 5e-9, kT = 1.380649e-23 * 300, Vt = kT / Q, h = 6.62607015e-34;
        const double Rd = V / global(sd, QStringLiteral("semi.I_2")), Rt = V / global(st, QStringLiteral("semi.I_2"));
        const double Rtc = V / global(*teCoarse.study.solutions.back(), QStringLiteral("semi.I_2")), Ru = V / global(su, QStringLiteral("semi.I_2"));
        // (The barrier's Nc in its bulk; its electrons at the interface.)
        const double Nc = at(st, QStringLiteral("semi.n"), 70, 2.5) * std::exp((at(st, QStringLiteral("semi.Ec"), 70, 2.5) - at(st, QStringLiteral("semi.Efn"), 70, 2.5)) / kT);
        const double m = std::pow(Nc / 2, 2.0 / 3.0) * h * h / (2 * M_PI * kT), vR = std::sqrt(kT / (2 * M_PI * m));
        const double n = at(st, QStringLiteral("semi.n"), 40.0001, 2.5);
        const double r = Vt / (Q * vR * n * A);
        qInfo("1 mV across: drift-diffusion %.6g Ω, thermionic %.6g Ω (a coarser mesh %.6g), with tunnelling %.6g Ω; the barrier's Nc %.4g /m³ "
              "(m* %.4f m0), v_R %.4g m/s, n' %.4g /m³: Vt/(q v_R n' A) %.6g Ω",
              Rd, Rt, Rtc, Ru, Nc, m / 9.1093837e-31, vR, n, r);
        QVERIFY2(std::abs((Rt - Rd) / r - 1) < std::max(0.02, within), qPrintable(QStringLiteral("%1 %2").arg(Rt - Rd).arg(r)));
        QVERIFY2(std::abs(Rtc / Rt - 1) < within, qPrintable(QStringLiteral("%1 %2").arg(Rtc).arg(Rt)));
        // Tunnelling: the interface's (Rt - Rd)/(1 + δ), δ at the field in
        // the barrier's first triangles, its step Ec's there.
        const double F = at(su, QStringLiteral("semi.normE"), 40.05, 2.5);
        const double step = at(su, QStringLiteral("semi.Ec"), 40.0001, 2.5) - at(su, QStringLiteral("semi.Ec"), 39.9999, 2.5);
        const double c = 4 * std::sqrt(2 * m) / (3 * Q * 1.054571817e-34 * F);
        double delta = 0;
        const int steps = 2000;
        for (int k = 0; k < steps; ++k) {
            const double u = (k + 0.5) * step / steps;
            delta += std::exp(u / kT - c * std::pow(u, 1.5)) * step / steps / kT;
        }
        qInfo("tunnelling: its field %.4g V/m, its step %.4f eV, δ %.4f; the interface %.6g Ω, (Rt - Rd)/(1 + δ) %.6g Ω", F, step / Q, delta, Ru - Rd,
              (Rt - Rd) / (1 + delta));
        QVERIFY2(delta > 0.1, qPrintable(QString::number(delta)));
        QVERIFY2(std::abs((Ru - Rd) / ((Rt - Rd) / (1 + delta)) - 1) < within, qPrintable(QStringLiteral("%1 %2").arg(Ru - Rd).arg((Rt - Rd) / (1 + delta))));
    }

    // Hot electrons where all is uniform: a bar of n-silicon (1e17 cm⁻³, 2
    // µm) at 1 V (5 kV/cm, 0.57 vsat), their temperature an energy
    // balance's (τw 0.3 ps). In its middle, far from its contacts' cool
    // electrons (some vsat τw), their heating (2/3) q τw v F/k, v and F the
    // bar's own there, to 1 %; its current the field-driven saturation's
    // to 1 %. (Nearer saturation the field settles along the bar slowly -
    // at 4 V 10 % above its mean in its middle; at 10 V the current is
    // past q Nd vsat, the electrons cooler than the field they enter a
    // little faster than vsat: the bar fills with 2.5 % more electrons
    // than its donors, its field rising along it, the energy carried
    // along the rise 4 % of the field's gift - (5/3) τw v F'/F.)
    void hotElectronsWhereUniform()
    {
        auto bar = [](const QString& driving) {
            return modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "label": "bar", "size": ["2", "0.2"]}],
                "materials": [{"material": "Silicon", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "model", "saturation": true, "driving": "%1", "selection": {"all": true}},
                  {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": true}},
                  {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}},
                  {"type": "ohmic", "name": "2", "V0": "1[V]", "selection": {"of": ["bar"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.02"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                               .arg(driving)
                               .toUtf8());
        };
        const Run hot = run(bar(QStringLiteral("energy"))), field = run(bar(QStringLiteral("field")));
        for (const Run* r : {&hot, &field}) QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
        const Solution& s = *hot.study.solutions.back();
        const double Ih = global(s, QStringLiteral("semi.I_2")), If = global(*field.study.solutions.back(), QStringLiteral("semi.I_2"));
        const double Tn = at(s, QStringLiteral("semi.Tn"), 1, 0.1), F = at(s, QStringLiteral("semi.normE"), 1, 0.1);
        const double v = std::abs(at(s, QStringLiteral("semi.Jnx"), 1, 0.1)) / (Q * at(s, QStringLiteral("semi.n"), 1, 0.1));
        const double expected = 300 + 2.0 / 3.0 * Q * 0.3e-12 * v * F / 1.380649e-23;
        qInfo("hot electrons at 1 V: %.6g A (driven by the field %.6g A); in the middle %.2f K (%.2f), v %.5g m/s, F %.5g V/m", Ih, If, Tn, expected, v, F);
        QVERIFY2(std::abs((Tn - 300) / (expected - 300) - 1) < 0.01, qPrintable(QStringLiteral("%1 %2").arg(Tn).arg(expected)));
        // Entering at the cathode, cool, they warm over 1/|λ|: κ λ² - (5/2) k
        // n v λ - (3/2) k n/τw = 0, κ = (5/2)(k²/q) µ n T - carried with their
        // current and conducted, some 60 nm (41 conducted alone): fitted to
        // its rise 30 and 60 nm in, to 10 %.
        const double kB = 1.380649e-23, nmid = at(s, QStringLiteral("semi.n"), 1, 0.1), mu = v / F;
        const double kappa = 2.5 * kB * kB / Q * mu * nmid * 0.5 * (300 + Tn), a = 2.5 * kB * nmid * v, c = 1.5 * kB * nmid / 0.3e-12;
        const double length = 2 * kappa / (std::sqrt(a * a + 4 * kappa * c) - a);
        const double d30 = Tn - at(s, QStringLiteral("semi.Tn"), 0.03, 0.1), d60 = Tn - at(s, QStringLiteral("semi.Tn"), 0.06, 0.1);
        const double fitted = 30e-9 / std::log(d30 / d60);
        qInfo("warming in: over %.4g nm (%.4g nm)", fitted * 1e9, length * 1e9);
        QVERIFY2(std::abs(fitted / length - 1) < 0.1, qPrintable(QStringLiteral("%1 %2").arg(fitted).arg(length)));
        QVERIFY2(std::abs(Ih / If - 1) < 0.01, qPrintable(QStringLiteral("%1 %2").arg(Ih).arg(If)));
    }

    // Hot electrons in time: the bar at 1 V stepped on from rest, 3 ps -
    // its temperatures solved with each step's Newton's method - its
    // current its rest's to 1e-4; its start's temperature and mobility
    // shown as at rest (they were setup's, 300 K and the low field's).
    void hotElectronsInTime()
    {
        const QByteArray json = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "label": "bar", "size": ["2", "0.2"]}],
                "materials": [{"material": "Silicon", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "model", "saturation": true, "driving": "energy", "selection": {"all": true}},
                  {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": true}},
                  {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}},
                  {"type": "ohmic", "name": "2", "V0": "1[V]", "selection": {"of": ["bar"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.04"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}, {"type": "transient", "times": "range(0, 1[ps], 3[ps])"}]}]})json")
                                    .toUtf8();
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.study.solutions.back();
        s.select(0);
        const double I0 = global(s, QStringLiteral("semi.I_2")), T0 = at(s, QStringLiteral("semi.Tn"), 1, 0.1);
        s.select(s.snapshotCount() - 1);
        const double I3 = global(s, QStringLiteral("semi.I_2")), T3 = at(s, QStringLiteral("semi.Tn"), 1, 0.1);
        s.select(0);
        const double mu0 = at(s, QStringLiteral("semi.mun"), 1, 0.1);
        s.select(s.snapshotCount() - 1);
        const double mu3 = at(s, QStringLiteral("semi.mun"), 1, 0.1);
        qInfo("in time: %.9g A at rest, %.9g A at 3 ps; %.3f K and %.3f K; mun %.6g and %.6g", I0, I3, T0, T3, mu0, mu3);
        QVERIFY2(std::abs(mu3 / mu0 - 1) < 1e-4, qPrintable(QStringLiteral("%1 %2").arg(mu3).arg(mu0)));
        QVERIFY2(std::abs(I3 / I0 - 1) < 1e-4, qPrintable(QStringLiteral("%1 %2").arg(I3).arg(I0)));
        QVERIFY2(std::abs(T3 - T0) < 0.1, qPrintable(QStringLiteral("%1 %2").arg(T3).arg(T0)));
    }

    // Velocity overshoot: n⁺-n-n⁺ silicon (1e19, 1e16 cm⁻³; 100, 200, 100
    // nm) at 3 V. The electrons entering the n layer from the cool n⁺ run
    // past vsat (1e5 m/s) with the energy balance - their temperature lags
    // the field by some vsat τw -, and no faster than it driven by the
    // field; their current the more (68.7 A against 41.6).
    void velocityOvershoot()
    {
        if (slowBuild()) QSKIP("hot carriers' turns: Release builds only (past 300 s sanitized)");
        auto nnn = [](const QString& driving) {
            return modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "label": "a", "size": ["0.1", "0.02"]},
                             {"type": "rectangle", "label": "n", "position": ["0.1", "0"], "size": ["0.2", "0.02"]},
                             {"type": "rectangle", "label": "b", "position": ["0.3", "0"], "size": ["0.1", "0.02"]}],
                "materials": [{"material": "Silicon", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "model", "saturation": true, "driving": "%1", "selection": {"all": true}},
                  {"type": "doping", "dopant": "donor", "N0": "1e19[1/cm^3]", "selection": {"only": ["a", "b"]}},
                  {"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"only": ["n"]}},
                  {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["a"], "side": "left"}},
                  {"type": "ohmic", "name": "2", "V0": "3[V]", "selection": {"of": ["b"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.004"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
                               .arg(driving)
                               .toUtf8());
        };
        const Run hot = run(nnn(QStringLiteral("energy"))), field = run(nnn(QStringLiteral("current")));
        for (const Run* r : {&hot, &field}) QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
        auto fastest = [](const Solution& s) {
            double most = 0;
            // (Inside the layer, 30 nm from its junctions: n beside them
            // a thousand times over a triangle, J/q n there no velocity.)
            for (int k = 0; k <= 140; ++k) {
                const double x = 0.13 + 0.14 * k / 140;
                most = std::max(most, std::abs(at(s, QStringLiteral("semi.Jnx"), x, 0.01)) / (Q * at(s, QStringLiteral("semi.n"), x, 0.01)));
            }
            return most;
        };
        const double vh = fastest(*hot.study.solutions.back()), vf = fastest(*field.study.solutions.back());
        const double Ih = global(*hot.study.solutions.back(), QStringLiteral("semi.I_2")), If = global(*field.study.solutions.back(), QStringLiteral("semi.I_2"));
        const double Tmax = at(*hot.study.solutions.back(), QStringLiteral("semi.Tn"), 0.3, 0.01);
        qInfo("n+ n n+ at 3 V: the fastest electrons %.4g m/s hot, %.4g m/s driven by the field; the current %.6g and %.6g A; %.1f K at the n layer's end",
              vh, vf, Ih, If, Tmax);
        QVERIFY2(vh > 1.2e5, qPrintable(QString::number(vh)));
        QVERIFY2(vf < 1.01e5, qPrintable(QString::number(vf)));
        QVERIFY2(Ih > If, qPrintable(QStringLiteral("%1 %2").arg(Ih).arg(If)));
    }

    // Transferred electrons: a bar of n-GaAs (1e16 cm⁻³, 10 µm), its
    // velocity (µ0 F + vsat (F/E0)⁴)/(1 + (F/E0)⁴), E0 4 kV/cm, vsat its
    // 7.2e6 cm/s, µ0 its own (at 10 mV): at 2, 4, 6 and 8 kV/cm its current
    // q n v A to 1e-3 - past the peak, at 6 and 8, less.
    void transferredElectrons()
    {
        const Run r = run(modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "parameters": [{"name": "V", "expression": "0.01[V]"}],
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "label": "bar", "size": ["10", "1"]}],
                "materials": [{"material": "Gallium arsenide", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "model", "saturation": true, "driving": "field", "velocity": "transferred", "selection": {"all": true}},
                  {"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"all": true}},
                  {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}},
                  {"type": "ohmic", "name": "2", "V0": "V", "selection": {"of": ["bar"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.5"}}],
              "studies": [{"type": "study", "steps": [{"type": "sweep", "parameters": [["V", "0.01, 2, 4, 6, 8", "V"]]}, {"type": "stationary"}]}]})json")
                                      .toUtf8()));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(int(r.study.solutions.size()), 5);
        const double n = 1e22, A = 1e-6, L = 10e-6, E0 = 4e5, vsat = 7.2e4;
        const double mu0 = global(*r.study.solutions[0], QStringLiteral("semi.I_2")) / (Q * n * A * 0.01 / L);
        QStringList said;
        double I[5];
        for (int k = 1; k < 5; ++k) {
            const double V = 2.0 * k, F = V / L, r4 = std::pow(F / E0, 4);
            const double expected = Q * n * A * (mu0 * F + vsat * r4) / (1 + r4);
            I[k] = global(*r.study.solutions[std::size_t(k)], QStringLiteral("semi.I_2"));
            said << QStringLiteral("%1 kV/cm %2 A (%3)").arg(F / 1e5).arg(I[k], 0, 'g', 6).arg(expected, 0, 'g', 6);
            QVERIFY2(std::abs(I[k] / expected - 1) < 1e-3, qPrintable(said.last()));
        }
        qInfo("transferred electrons, µ0 %.5g m²/(V s): %s", mu0, qPrintable(said.join(QLatin1String("; "))));
        QVERIFY2(I[3] < I[2] && I[4] < I[3], qPrintable(said.join(QLatin1String("; "))));
    }

    // A global's harmonics in time: a silicon bar (1e17, 2 µm) driven by 10
    // mV at 1 GHz and 2 mV at 2 GHz - Ohm's alone at these fields -: its
    // current's mean nought, H1 and H2 the voltages over its resistance (at
    // 10 mV at rest), HD2 20 log10(0.2) = -13.98 dBc, H3 none; THD 0.2.
    void harmonicsOfABar()
    {
        if (slowBuild()) QSKIP("in time: Release builds only (minutes sanitized)");
        auto bar = [](const QString& V0, const QString& steps) {
            return modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "label": "bar", "size": ["2", "0.2"]}],
                "materials": [{"material": "Silicon", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [
                  {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": true}},
                  {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}},
                  {"type": "ohmic", "name": "2", "V0": "%1", "selection": {"of": ["bar"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.1"}}],
              "studies": [{"type": "study", "steps": [%2]}]})json")
                               .arg(V0, steps)
                               .toUtf8());
        };
        const Run rest = run(bar(QStringLiteral("10[mV]"), QStringLiteral(R"({"type": "stationary"})")));
        QVERIFY2(rest.error.isEmpty(), qPrintable(rest.error));
        const double R = 0.01 / global(*rest.study.solutions.back(), QStringLiteral("semi.I_2"));
        const Run r = run(bar(QStringLiteral("10[mV]*sin(2*pi*1[GHz]*t) + 2[mV]*sin(2*pi*2[GHz]*t)"),
                              QStringLiteral(R"x({"type": "stationary"}, {"type": "transient", "times": "range(0, 25[ps], 2[ns])", "stepping": "fixed", "dt": "5[ps]"})x")));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Node harm;
        harm.type = QStringLiteral("harmonics");
        harm.props.insert(QStringLiteral("expression"), QStringLiteral("semi.I_2"));
        harm.props.insert(QStringLiteral("f0"), QStringLiteral("1[GHz]"));
        harm.props.insert(QStringLiteral("count"), 3);
        harm.props.insert(QStringLiteral("periods"), 1);
        Solution& sol = *r.study.solutions.back();
        sol.select(sol.snapshotCount() / 2);
        const double before = global(sol, QStringLiteral("semi.I_2"));
        const DerivedResult d = evaluateDerived(sol, harm);
        QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
        // (The snapshot selected as it was: its globals its own again.)
        QCOMPARE(global(sol, QStringLiteral("semi.I_2")), before);
        std::map<QString, double> v;
        QStringList said;
        for (const DerivedValue& x : d.values) {
            v[x.name] = x.value;
            said << x.name + QLatin1Char(' ') + x.text;
        }
        qInfo("the bar's current, R %.6g Ω: %s (%s)", R, qPrintable(said.join(QLatin1String(", "))), qPrintable(d.note));
        QVERIFY2(std::abs(v[QStringLiteral("H1")] * R / 0.01 - 1) < 1e-3, qPrintable(QString::number(v[QStringLiteral("H1")] * R)));
        QVERIFY2(std::abs(v[QStringLiteral("H2")] * R / 0.002 - 1) < 1e-3, qPrintable(QString::number(v[QStringLiteral("H2")] * R)));
        QVERIFY2(std::abs(v[QStringLiteral("HD2")] - 20 * std::log10(0.2)) < 0.01, qPrintable(QString::number(v[QStringLiteral("HD2")])));
        QVERIFY2(v[QStringLiteral("H3")] < 1e-4 * v[QStringLiteral("H1")], qPrintable(QString::number(v[QStringLiteral("H3")])));
        QVERIFY2(std::abs(v[QStringLiteral("mean")]) < 1e-4 * v[QStringLiteral("H1")], qPrintable(QString::number(v[QStringLiteral("mean")])));
        QVERIFY2(std::abs(v[QStringLiteral("THD")] / 0.2 - 1) < 2e-3, qPrintable(QString::number(v[QStringLiteral("THD")])));
        // Its times not on the window's start (32 ps to 2.24 ns: the window
        // from 1.24 ns, between two, the current near its peak): its first
        // value interpolated there - to 5e-3 (the trapezoids' ends uneven).
        const Run shifted = run(bar(QStringLiteral("10[mV]*sin(2*pi*1[GHz]*t) + 2[mV]*sin(2*pi*2[GHz]*t)"),
                                    QStringLiteral(R"x({"type": "stationary"}, {"type": "transient", "times": "range(0, 32[ps], 2.24[ns])", "stepping": "fixed", "dt": "5[ps]"})x")));
        QVERIFY2(shifted.error.isEmpty(), qPrintable(shifted.error));
        const DerivedResult e = evaluateDerived(*shifted.study.solutions.back(), harm);
        QVERIFY2(e.error.isEmpty() && e.values.size() >= 3, qPrintable(e.error));
        qInfo("its times off the window: H1 %s, H2 %s", qPrintable(e.values[1].text), qPrintable(e.values[2].text));
        QVERIFY2(std::abs(e.values[1].value * R / 0.01 - 1) < 5e-3, qPrintable(QString::number(e.values[1].value * R)));
        QVERIFY2(std::abs(e.values[2].value * R / 0.002 - 1) < 5e-3, qPrintable(QString::number(e.values[2].value * R)));
    }

    // A photodiode's distortion and compression: the p-i-n lit 50 %
    // modulated at 1 GHz through 100 kΩ in series at -2 V - weakly (10 nA's
    // worth) linear, HD2 -92 dBc; strongly (20 µA's worth, its drop all but
    // the bias) compressed: its fundamental per pair 0.30 of the weak one's,
    // HD2 -19 dBc, HD3 -37.
    void aPhotodiodesDistortion()
    {
        if (slowBuild()) QSKIP("in time: Release builds only (minutes sanitized)");
        auto lit = [](const QString& G0) {
            QByteArray j = pin(QStringLiteral(R"(, "circuit": "series", "Rs": "100[kOhm]", "RL": "0[Ohm]")"),
                               QStringLiteral(R"x({"type": "stationary"}, {"type": "transient", "times": "range(0, 25[ps], 3[ns])", "stepping": "fixed", "dt": "5[ps]"})x"));
            j.replace("6e29[1/(m^3*s)]", G0.toUtf8());
            j.replace(R"("G": "G0",)", R"x("G": "G0*(1 + 0.5*sin(2*pi*1[GHz]*t))",)x");
            return j;
        };
        Node harm;
        harm.type = QStringLiteral("harmonics");
        harm.props.insert(QStringLiteral("expression"), QStringLiteral("semi.I_a"));
        harm.props.insert(QStringLiteral("f0"), QStringLiteral("1[GHz]"));
        harm.props.insert(QStringLiteral("count"), 3);
        std::map<QString, double> v[2];
        const char* G0[2] = {"6e27[1/(m^3*s)]", "1.2e31[1/(m^3*s)]"};
        for (int k = 0; k < 2; ++k) {
            const Run r = run(modelOf(lit(QString::fromLatin1(G0[k]))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const DerivedResult d = evaluateDerived(*r.study.solutions.back(), harm);
            QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
            QStringList said;
            for (const DerivedValue& x : d.values) {
                v[k][x.name] = x.value;
                said << x.name + QLatin1Char(' ') + x.text;
            }
            qInfo("G0 %s: %s", G0[k], qPrintable(said.join(QLatin1String(", "))));
        }
        const double perPair = (v[1][QStringLiteral("H1")] / 1.2e31) / (v[0][QStringLiteral("H1")] / 6e27);
        qInfo("strong light's fundamental per pair %.4f of the weak's", perPair);
        QVERIFY2(v[0][QStringLiteral("HD2")] < -50, qPrintable(QString::number(v[0][QStringLiteral("HD2")])));
        QVERIFY2(v[1][QStringLiteral("HD2")] > -20, qPrintable(QString::number(v[1][QStringLiteral("HD2")])));
        QVERIFY2(perPair < 0.8, qPrintable(QString::number(perPair)));
    }

    // Noise by the impedance field. A bar of n-silicon (1e17, 2 µm) at
    // equilibrium: its noise current Nyquist's, 4kT Re Y, at 1 MHz.
    void thermalNoiseOfABar()
    {
        const Run r = run(modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "label": "bar", "size": ["2", "0.2"]}],
                "materials": [{"material": "Silicon", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "noise": true, "features": [
                  {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": true}},
                  {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}},
                  {"type": "ohmic", "name": "2", "V0": "0[V]", "selection": {"of": ["bar"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.05"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}, {"type": "frequency", "frequencies": "1[MHz]"}]}]})json")
                                      .toUtf8()));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.study.solutions.back();
        const double S = global(s, QStringLiteral("semi.Sn_2")), G = global(s, QStringLiteral("semi.G22")), kT = 1.380649e-23 * 300;
        qInfo("at equilibrium: %.9g A²/Hz, 4kT G %.9g (G %.6g S)", S, 4 * kT * G, G);
        QVERIFY2(std::abs(S / (4 * kT * G) - 1) < 1e-3, qPrintable(QStringLiteral("%1 %2").arg(S).arg(4 * kT * G)));
        // At 3 V across (Ohm's, its mobility the low field's): its noise
        // still its thermal noise, 4kT I/V, whatever its current - its
        // diffusion's, not its drift's (each edge's two streams as hops:
        // some twice as much) - to 1 %: 0.4 % above, as the field squared
        // (at 10 V 3.8 %).
        QByteArray biased = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "label": "bar", "size": ["2", "0.2"]}],
                "materials": [{"material": "Silicon", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "noise": true, "features": [
                  {"type": "doping", "dopant": "donor", "N0": "1e17[1/cm^3]", "selection": {"all": true}},
                  {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}},
                  {"type": "ohmic", "name": "2", "V0": "3[V]", "selection": {"of": ["bar"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.05"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}, {"type": "frequency", "frequencies": "1[MHz]"}]}]})json")
                                    .toUtf8();
        const Run b = run(modelOf(biased));
        QVERIFY2(b.error.isEmpty(), qPrintable(b.error));
        const double Sb = global(*b.study.solutions.back(), QStringLiteral("semi.Sn_2")), Ib = global(*b.study.solutions.back(), QStringLiteral("semi.I_2"));
        qInfo("at 3 V: %.6g A, %.9g A²/Hz, 4kT I/V %.9g (%.4f)", Ib, Sb, 4 * kT * Ib / 3, Sb / (4 * kT * Ib / 3));
        QVERIFY2(std::abs(Sb / (4 * kT * Ib / 3) - 1) < 1e-2, qPrintable(QStringLiteral("%1 %2").arg(Sb).arg(4 * kT * Ib / 3)));
    }

    // A photodiode's shot noise: the p-i-n reversed 2 V, lit (some 10 µA's
    // worth): its noise 2q I at 1 MHz - each pair collected once.
    void shotNoiseOfAPhotodiode()
    {
        QByteArray j = pin(QString(), QStringLiteral(R"({"type": "stationary"}, {"type": "frequency", "frequencies": "1[MHz]"})"));
        j.replace("6e29[1/(m^3*s)]", "6e30[1/(m^3*s)]");
        j.replace(R"("tag": "semi", "features": [)", R"("tag": "semi", "noise": true, "features": [)");
        const Run r = run(modelOf(j));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Solution& s = *r.study.solutions.back();
        const double S = global(s, QStringLiteral("semi.Sn_a")), I = global(s, QStringLiteral("semi.I_a")), F = global(s, QStringLiteral("semi.Fshot_a"));
        qInfo("lit at -2 V: %.6g A, %.6g A²/Hz, 2q I %.6g (%.5f)", I, S, 2 * Q * std::abs(I), F);
        QVERIFY2(std::abs(F - 1) < 0.01, qPrintable(QString::number(F)));
        // Through 50 Ω in series - far less than the device's impedance at 1
        // MHz -: the load's the same.
        QByteArray loaded = j;
        loaded.replace(R"("V0": "Va", "selection": {"of": ["p"], "side": "left"})",
                       R"("V0": "Va", "selection": {"of": ["p"], "side": "left"}, "circuit": "series", "Rs": "0[Ohm]", "RL": "50[Ohm]")");
        const Run l = run(modelOf(loaded));
        QVERIFY2(l.error.isEmpty(), qPrintable(l.error));
        const double SL = global(*l.study.solutions.back(), QStringLiteral("semi.SnL_a"));
        qInfo("through 50 Ω: %.6g A²/Hz", SL);
        QVERIFY2(std::abs(SL / S - 1) < 1e-3, qPrintable(QStringLiteral("%1 %2").arg(SL).arg(S)));
        // Germanium on silicon at 1550 nm, 10 µW: its noise-equivalent
        // power Pin √(2q/I) at 1 MHz (its noise 2q I, its responsivity I/Pin).
        QByteArray g = waveguidePd(1, QStringLiteral("10[uW]"), QString(), QStringLiteral(R"(, {"type": "stationary", "physics": ["semi"]}, {"type": "frequency", "physics": ["semi"], "frequencies": "1[MHz]"})"), true);
        g.replace(R"("tag": "semi", "selection")", R"("tag": "semi", "noise": true, "selection")");
        const Run w = run(modelOf(g));
        QVERIFY2(w.error.isEmpty(), qPrintable(w.error));
        const Solution& ws = *w.study.solutions.back();
        const double Ik = std::abs(global(ws, QStringLiteral("semi.I_k"))), NEP = global(ws, QStringLiteral("semi.NEP_k"));
        const double expected = 10e-6 * std::sqrt(2 * Q / Ik);
        qInfo("germanium, 10 µW: %.6g A, NEP %.6g W/√Hz (%.6g)", Ik, NEP, expected);
        QVERIFY2(std::abs(NEP / expected - 1) < 0.01, qPrintable(QStringLiteral("%1 %2").arg(NEP).arg(expected)));
    }

    // An APD's excess noise: silicon p⁺-i-n⁺ (its i layer 2 µm of 1e13
    // cm⁻³, its field all but uniform), lit in its p⁺ alone - electrons
    // injected -, van Overstraeten and de Man's ionization, reversed so its
    // gain is some 5. Its noise 2q I M F at 1 MHz, F McIntyre's, k M + (1
    // - k)(2 - 1/M), k = αp/αn at its field, M its current over the same
    // device's without ionization - to 3 % on elements of 5 nm (F 4.03,
    // 2.92, 2.39, 2.10 at 40, 20, 10, 5 nm; McIntyre's 2.10).
    void excessNoiseOfAnApd()
    {
        if (slowBuild()) QSKIP("an avalanche's walk: Release builds only");
        auto apd = [](const QString& ii) {
            return modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "parameters": [{"name": "Va", "expression": "-52[V]"}],
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "label": "p", "size": ["0.3", "0.05"]},
                             {"type": "rectangle", "label": "i", "position": ["0.3", "0"], "size": ["2", "0.05"]},
                             {"type": "rectangle", "label": "n", "position": ["2.3", "0"], "size": ["0.3", "0.05"]}],
                "materials": [{"material": "Silicon", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "noise": true, "features": [
                  {"type": "doping", "dopant": "acceptor", "N0": "1e19[1/cm^3]", "selection": {"only": ["p"]}},
                  {"type": "doping", "dopant": "donor", "N0": "1e13[1/cm^3]", "selection": {"only": ["i"]}},
                  {"type": "doping", "dopant": "donor", "N0": "1e19[1/cm^3]", "selection": {"only": ["n"]}},
                  {"type": "srh", "selection": {"all": true}},
                  {"type": "generation", "G": "1e29[1/(m^3*s)]", "selection": {"only": ["p"]}} %1,
                  {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["p"], "side": "left"}},
                  {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["n"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.005"}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary"}, {"type": "frequency", "frequencies": "1[MHz]"}]}]})json")
                               .arg(ii)
                               .toUtf8());
        };
        const Run with = run(apd(QStringLiteral(R"(, {"type": "impact", "model": "silicon", "selection": {"all": true}})")));
        const Run without = run(apd(QString()));
        for (const Run* r : {&with, &without}) QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
        const Solution& s = *with.study.solutions.back();
        const double I = std::abs(global(s, QStringLiteral("semi.I_k"))), I0 = std::abs(global(*without.study.solutions.back(), QStringLiteral("semi.I_k")));
        const double Fshot = global(s, QStringLiteral("semi.Fshot_k")), M = I / I0, F = Fshot / M;
        double E = 0;
        for (int k = 0; k < 20; ++k) E += at(s, QStringLiteral("semi.normE"), 0.35 + 1.9 * (k + 0.5) / 20, 0.025) / 20;
        const double an = 7.03e7 * std::exp(-1.231e8 / E), ap = E < 4e7 ? 1.582e8 * std::exp(-2.036e8 / E) : 6.71e7 * std::exp(-1.693e8 / E);
        const double k = ap / an, mcintyre = k * M + (1 - k) * (2 - 1 / M);
        qInfo("APD: M %.4f, its field %.4g V/m (k %.4f); its noise over 2q I %.4f, F %.4f, McIntyre's %.4f", M, E, k, Fshot, F, mcintyre);
        QVERIFY2(M > 3, qPrintable(QString::number(M)));
        QVERIFY2(std::abs(F / mcintyre - 1) < 0.03, qPrintable(QStringLiteral("%1 %2").arg(F).arg(mcintyre)));
    }

    // The avalanche diode's wall (the APD round's, 18 October): a one-sided
    // silicon junction (p⁺ 1e19, n 1e16 over 4 µm, its breakdown 51.7 V),
    // velocity saturation along the current, impact ionization, lit beyond
    // its depletion (1e24 pairs/(m³ s) past 3 µm), meshed at 0.1 µm. Its
    // sweep found no solution past -22.7 V (Fermi-Dirac's) or -25 V
    // (Boltzmann's): one edge's two triangles' cotangents all but cancelled,
    // its face 10⁻¹⁵ of its neighbours', and their mobilities, saturated
    // apart, made its current density - the ionization's fit's - anything:
    // a triangle's holes' current 7·10⁵ A/m² in a diode carrying 0.04, the
    // electrons beside it no solution but a negative one. Now each edge's
    // mobility there no more than its triangles' largest.
    // Swept to -50 V: its multiplication, the current over the same diode's
    // without its pairs in the continuity, McIntyre's for holes entering
    // the field, 1/(1 - Ip), its ionization integral (semi.Mp).
    void anAvalanchePastTheWall()
    {
        auto diode = [](bool fermi, bool currents) {
            return modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "Va", "expression": "0[V]"}],
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "label": "pplus", "position": ["-0.5", "0"], "size": ["0.5", "0.2"]},
                     {"type": "rectangle", "label": "nside", "position": ["0", "0"], "size": ["4", "0.2"]}],
        "materials": [{"material": "Silicon", "selection": {"all": true}}],
        "physics": [{"type": "semiconductor", "tag": "semi", "features": [
          {"type": "model", "saturation": true, "driving": "current", "fermi": %1, "selection": {"all": true}},
          {"type": "doping", "dopant": "acceptor", "N0": "1e19[1/cm^3]", "selection": {"only": ["pplus"]}},
          {"type": "doping", "dopant": "donor", "N0": "1e16[1/cm^3]", "selection": {"only": ["nside"]}},
          {"type": "impact", "currents": %2, "selection": {"all": true}},
          {"type": "generation", "G": "1e24[1/(m^3*s)]*(x > 3[um])", "selection": {"all": true}},
          {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["pplus"], "side": "left"}},
          {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["nside"], "side": "right"}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "0.1"}}],
      "studies": [{"type": "study", "steps": [{"type": "sweep", "parameters": [["Va", "-10, -20, -25, -30, -40, -50", "V"]]}, {"type": "stationary"}]}]})json")
                               .arg(fermi ? QLatin1String("true") : QLatin1String("false"), currents ? QLatin1String("true") : QLatin1String("false"))
                               .toUtf8());
        };
        // (A sanitized build: Boltzmann's alone - 114 s for the three
        // sweeps, two thirds of it.)
        const bool slow = slowBuild();
        const Run boltzmann = run(diode(false, true)), bare = run(diode(false, false));
        const Run fermi = slow ? Run() : run(diode(true, true));
        for (const Run* r : {&boltzmann, &fermi, &bare}) {
            if (r == &fermi && slow) continue;
            QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
            QVERIFY2(r->study.stopped.isEmpty(), qPrintable(r->study.stopped));
            QCOMPARE(r->study.solutions.size(), std::size_t(6));
        }
        static const double volts[6] = {-10, -20, -25, -30, -40, -50};
        for (int k = 0; k < 6; ++k) {
            const double I = std::abs(global(*boltzmann.study.solutions[std::size_t(k)], QStringLiteral("semi.I_k")));
            const double If = slow ? I : std::abs(global(*fermi.study.solutions[std::size_t(k)], QStringLiteral("semi.I_k")));
            const double I0 = std::abs(global(*bare.study.solutions[std::size_t(k)], QStringLiteral("semi.I_k")));
            const double Mp = global(*boltzmann.study.solutions[std::size_t(k)], QStringLiteral("semi.Mp"));
            const double M = I / I0;
            qInfo("at %g V: %.6g A (Fermi-Dirac's %.6g), without its pairs %.6g A: M %.4f, McIntyre's Mp %.4f", volts[k], I, If, I0, M, Mp);
            // (To -40 V the two to 1 %; at -50 V, a volt and a half from
            // breakdown, M - its ionization integral 1 - 1/M - to 3 %: 5.87
            // against 6.65, 2.4 % in the integral, 1.0 % at 0.05 µm, 0.3 % at
            // 0.025.) Fermi-Dirac's, its n side not degenerate, the same.
            if (k < 5) QVERIFY2(std::abs(M / Mp - 1) < 0.01, qPrintable(QStringLiteral("%1 V: %2 %3").arg(volts[k]).arg(M).arg(Mp)));
            else {
                QVERIFY2(M > 5, qPrintable(QString::number(M)));
                QVERIFY2(std::abs((1 - 1 / M) / (1 - 1 / Mp) - 1) < 0.03, qPrintable(QStringLiteral("%1 %2").arg(M).arg(Mp)));
            }
            QVERIFY2(std::abs(If / I - 1) < 0.01, qPrintable(QStringLiteral("%1 V: %2 %3").arg(volts[k]).arg(If).arg(I)));
        }
    }
};

QTEST_GUILESS_MAIN(TestFemPhotodiodePower)
#include "test_fem_photodiode_power.moc"
