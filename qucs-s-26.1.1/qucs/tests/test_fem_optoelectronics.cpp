/*
 * test_fem_optoelectronics.cpp - the multiphysics solver's light in
 *                                semiconductors: Airy's functions and the
 *                                Franz-Keldysh effect; a germanium waveguide
 *                                photodiode (its mode's light made pairs, its
 *                                photocurrent, its response modulated); an
 *                                electroabsorption modulator (its mode's loss
 *                                with the reverse bias)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_optics.h"
#include "fem_results.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QtTest>

#include <cmath>
#include <vector>

using namespace qucs_s::fem;

namespace {

constexpr double Q = 1.602176634e-19, H = 6.62607015e-34, C0 = 299792458.0;

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

Run run(const Model& model, const QString& study = {})
{
    Run r;
    r.parameters = evaluateParameters(model);
    const GeometryBuild g = buildGeometry(model, *r.parameters);
    if (!g.ok()) {
        r.error = QStringLiteral("geometry: ") + QStringList(g.errors.values()).join(QLatin1String("; "));
        return r;
    }
    r.study = runStudy(model, r.parameters, g.topology, nullptr, study);
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

/// A germanium stripe on a silicon slab (µm, in oxide), p⁺ and n⁺ at its
/// sides, contacts on them: a lateral pin across a waveguide - a
/// photodiode's (thickness its length) or a modulator's. \a material its
/// germanium (and what it overrides), \a features the semiconductor's more,
/// \a studies.
QByteArray stripe(const QString& length, const QString& material, const QString& wide, const QString& features, const QString& studies)
{
    // (A sanitized build: coarser - its mode solves take minutes there.)
    const QString hmax = slowBuild() ? QStringLiteral("0.1") : QStringLiteral("0.04");
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "parameters": [{"name": "Va", "expression": "-1[V]"}, {"name": "P", "expression": "1[mW]"}, {"name": "w", "expression": "%3"}],
      "components": [{"unit": "um", "thickness": "%1",
        "geometry": [{"type": "rectangle", "label": "box", "position": ["-2", "-1"], "size": ["4", "1"]},
                     {"type": "rectangle", "label": "cladding", "position": ["-2", "0"], "size": ["4", "1.6"]},
                     {"type": "rectangle", "label": "slab", "position": ["-1.5", "0"], "size": ["3", "0.22"]},
                     {"type": "rectangle", "label": "pplus", "position": ["-w/2 - 0.2", "0.22"], "size": ["0.2", "0.45"]},
                     {"type": "rectangle", "label": "absorber", "position": ["-w/2", "0.22"], "size": ["w", "0.45"]},
                     {"type": "rectangle", "label": "nplus", "position": ["w/2", "0.22"], "size": ["0.2", "0.45"]},
                     {"type": "form union"}],
        "materials": [{"material": "Silicon dioxide", "selection": {"only": ["box", "cladding"]}},
                      {"material": "Silicon", "selection": {"only": ["slab"]}},
                      {%2, "selection": {"only": ["pplus", "absorber", "nplus"]}}],
        "physics": [{"type": "semiconductor", "tag": "semi", "selection": {"only": ["slab", "pplus", "absorber", "nplus"]}, "features": [
                       {"type": "model", "saturation": true, "vsatn": "6e6[cm/s]", "vsatp": "6e6[cm/s]", "selection": {"only": ["pplus", "absorber", "nplus"]}},
                       {"type": "doping", "dopant": "acceptor", "N0": "1e19[1/cm^3]", "selection": {"only": ["pplus"]}},
                       {"type": "doping", "dopant": "donor", "N0": "1e19[1/cm^3]", "selection": {"only": ["nplus"]}},
                       {"type": "doping", "dopant": "acceptor", "N0": "1e15[1/cm^3]", "selection": {"only": ["slab", "absorber"]}},
                       {"type": "srh", "selection": {"all": true}},
                       %4
                       {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["pplus"], "side": "top"}},
                       {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["nplus"], "side": "top"}}]},
                    {"type": "modes", "tag": "emw", "features": [
                       {"type": "wave", "electroabsorption": true, "selection": {"only": ["pplus", "absorber", "nplus"]}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "0.25",
                 "features": [{"type": "size", "level": "domain", "selection": {"only": ["pplus", "absorber", "nplus", "slab"]}, "hmax": "%6"}]}}],
      "studies": [%5]})json")
        .arg(length, material, wide, features, studies, hmax)
        .toUtf8();
}

} // namespace

class TestFemOptoelectronics : public QObject
{
    Q_OBJECT

private slots:
    // Airy's functions against SciPy's (to 1e-7, where the series and the
    // expansions meet the least); the Franz-Keldysh absorption against
    // SciPy's evaluation of its formula, its limits: no field, the edge's
    // √(ħω - Eg)/ħω; a weak one, near it; far below the gap, nothing.
    void airyAndFranzKeldysh()
    {
        struct Value {
            double x, ai, aip;
        };
        const Value table[] = {{0, 0.3550280538878172, -0.2588194037928068},     {1, 0.13529241631288147, -0.15914744129679328},
                               {2, 0.03492413042327436, -0.05309038443365388},   {3.5, 0.0025840987869896357, -0.005004413967952583},
                               {4, 0.0009515638512048024, -0.00195864095020418}, {4.5, 0.00033025032351430934, -0.0007178665675575097},
                               {5, 1.0834442813607433e-4, -2.4741389086846237e-4}, {5.5, 3.368531190859981e-05, -8.046339130556513e-05},
                               {6, 9.947694360252897e-06, -2.4765200397034972e-05}, {8, 4.692207616099224e-08, -1.3414392979067844e-07},
                               {10, 1.1047532552898654e-10, -3.520633676738912e-10}, {-1, 0.5355608832923522, -0.010160567116645175},
                               {-2, 0.22740742820168564, 0.618259020741691},     {-3.5, -0.375533823140432, -0.3434434334540481},
                               {-5, 0.3507610090241143, 0.3271928185544436},     {-8, -0.05270505035638643, 0.9355609381983064},
                               {-10, 0.04024123848644196, 0.9962650441327905},   {-20, -0.17640612707798434, 0.8928628567364726},
                               {-6.5, -0.2380203019971157, -0.6749524925132023}};
        double worst = 0;
        for (const Value& v : table) {
            double ai = 0, aip = 0;
            airy(v.x, &ai, &aip);
            // (Relative, or of the function's size where it oscillates through nought.)
            const double scale = v.x < 0 ? std::pow(std::abs(v.x), 0.25) : 1.0;
            const double eAi = std::abs(ai - v.ai) / std::max(std::abs(v.ai), v.x < 0 ? 0.5 / scale : 0.0);
            const double eAip = std::abs(aip - v.aip) / std::max(std::abs(v.aip), v.x < 0 ? 0.5 * scale : 0.0);
            QVERIFY2(eAi < 1e-7 && eAip < 1e-7, qPrintable(QStringLiteral("x %1: Ai %2 (%3), Ai' %4 (%5)").arg(v.x).arg(ai, 0, 'g', 16).arg(v.ai, 0, 'g', 16)
                                                                .arg(aip, 0, 'g', 16).arg(v.aip, 0, 'g', 16)));
            worst = std::max({worst, eAi, eAip});
        }
        const double eV = Q, A = 2e6, mr = 0.03, gap = 0.80 * eV;
        struct Absorption {
            double photon, field, alpha;
        };
        const Absorption fk[] = {{0.78, 1e7, 53812.60185110592}, {0.78, 5e6, 25119.109488652848}, {0.85, 1e7, 473012.2625599522},
                                 {0.85, 1e3, 526137.3689126715}, {0.80, 1e7, 117955.21862922497}};
        for (const Absorption& a : fk) {
            const double alpha = franzKeldysh(a.photon * eV, gap, a.field, mr, A);
            QVERIFY2(std::abs(alpha / a.alpha - 1) < 1e-6, qPrintable(QStringLiteral("%1 eV, %2 V/m: %3 (%4)").arg(a.photon).arg(a.field).arg(alpha).arg(a.alpha)));
        }
        // Broadened (σ), against SciPy's quad of the Gaussian's mean.
        const Absorption spread[] = {{0.80, 0, 706.3790243494632}, {0.80, 6e6, 34387.62414304693}, {0.85, 0, 523391.3371403384}, {0.78, 1e7, 54890.54760188467}};
        const double gaps[] = {0.82, 0.82, 0.80, 0.80}, sigmas[] = {0.008, 0.008, 0.01, 0.005};
        for (int k = 0; k < 4; ++k) {
            const Absorption& a = spread[k];
            const double alpha = franzKeldysh(a.photon * eV, gaps[k] * eV, a.field, mr, A, sigmas[k] * eV);
            QVERIFY2(std::abs(alpha / a.alpha - 1) < 2e-3,
                     qPrintable(QStringLiteral("broadened %1 eV, %2 V/m: %3 (%4)").arg(a.photon).arg(a.field).arg(alpha).arg(a.alpha)));
        }
        const double edge = A * std::sqrt(0.05) / 0.85;
        QCOMPARE(franzKeldysh(0.85 * eV, gap, 0, mr, A), edge);
        QCOMPARE(franzKeldysh(0.78 * eV, gap, 0, mr, A), 0.0);
        QVERIFY(franzKeldysh(0.60 * eV, gap, 1e7, mr, A) < 1e-3 * franzKeldysh(0.78 * eV, gap, 1e7, mr, A));
        QVERIFY(franzKeldysh(0.78 * eV, gap, 2e7, mr, A) > franzKeldysh(0.78 * eV, gap, 1e7, mr, A));
        qInfo("Airy: within %.2g of SciPy's; Franz-Keldysh: 20 meV below the gap %.4g /cm at 100 kV/cm, %.4g at 50; the edge 50 meV above "
              "%.5g /cm without a field (A √0.05 / 0.85 %.5g), %.5g at 10 V/cm",
              worst, franzKeldysh(0.78 * eV, gap, 1e7, mr, A) / 100, franzKeldysh(0.78 * eV, gap, 5e6, mr, A) / 100,
              franzKeldysh(0.85 * eV, gap, 0, mr, A) / 100, edge / 100, franzKeldysh(0.85 * eV, gap, 1e3, mr, A) / 100);
    }

    // A germanium-on-silicon waveguide photodiode, 15 µm long, 1 mW at
    // 1550 nm. The mode's loss all absorbed across the gap - its power's
    // fall (Im β) and ∫ ω ε0 n κ |E|² the same (Poynting's theorem). The
    // pairs made, the photons the device absorbs: P (1 - e^(-2αL))/hν. Lit,
    // the current more by what is collected of q Gop; modulated, at low
    // frequency the same share, at 100 GHz less (the carriers' transit),
    // through 50 Ω a little less again. Dark: no light, no pairs.
    void aGermaniumPhotodiode()
    {
        QElapsedTimer timer;
        timer.start();
        const QString light = QStringLiteral(R"({"type": "optical", "mode": "emw", "P0": "P", "selection": {"only": ["pplus", "absorber", "nplus"]}},)");
        const Model model = modelOf(stripe(QStringLiteral("15[um]"), QStringLiteral(R"("material": "Germanium on silicon")"), QStringLiteral("0.7"), light,
                                           QStringLiteral(R"({"type": "study", "tag": "dark", "steps": [{"type": "stationary", "physics": ["semi"]}]},
              {"type": "study", "tag": "lit", "steps": [{"type": "stationary", "physics": ["semi"]},
                {"type": "mode", "physics": ["emw"], "frequency": "1550[nm]", "modes": 1, "neff": "3.9"},
                {"type": "stationary", "physics": ["semi"]},
                {"type": "frequency", "physics": ["semi"], "frequencies": "1[MHz], 100[GHz]"}]})")));
        const Run dark = run(model, QStringLiteral("dark"));
        QVERIFY2(dark.error.isEmpty(), qPrintable(dark.error));
        const double Idark = global(*dark.solution, QStringLiteral("semi.I_k"));
        QCOMPARE(global(*dark.solution, QStringLiteral("semi.Gop")), 0.0);
        const Run lit = run(model, QStringLiteral("lit"));
        QVERIFY2(lit.error.isEmpty(), qPrintable(lit.error));
        const auto& solutions = lit.study.solutions;
        QCOMPARE(int(solutions.size()), 1);
        Solution& s = *solutions.front();
        // (The frequency step's own series, not after the mode's at 193 THz;
        // the mode's fields and globals in each.)
        QCOMPARE(s.snapshotCount(), 2);
        QVERIFY(!s.modeAnalysis);
        QCOMPARE(s.snapshotFrequency(0), 1e6);
        s.select(0);   // (1 MHz)
        const double loss = global(s, QStringLiteral("emw.loss")), lossGap = global(s, QStringLiteral("emw.loss_gap"));
        const double alpha = global(s, QStringLiteral("emw.alpha")), L = 15e-6, P = 1e-3, photon = H * C0 / 1550e-9;
        const double Gop = global(s, QStringLiteral("semi.Gop")), Iop = global(s, QStringLiteral("semi.Iop"));
        const double Ilit = global(s, QStringLiteral("semi.I_k")), collected = (Ilit - Idark) / Iop;
        const double hLow = global(s, QStringLiteral("semi.Hop_k")), hLowRL = global(s, QStringLiteral("semi.HopRL_k"));
        s.select(1);   // (100 GHz)
        const double hHigh = global(s, QStringLiteral("semi.Hop_k")), hHighRL = global(s, QStringLiteral("semi.HopRL_k"));
        const double expected = P * -std::expm1(-2 * alpha * L) / photon;
        qInfo("Ge photodiode: its mode's loss %.6g dB/cm, across the gap %.6g; pairs %.5g /s (P (1 - e^-2αL)/hν %.5g, 2αL %.3g); dark %.4g nA, "
              "lit %.5g mA (%.4f A/W), collected %.4f of q Gop; modulated %.4f at 1 MHz (%.4f through 50 Ω), %.4f at 100 GHz (%.4f); %lld ms",
              loss / 100, lossGap / 100, Gop, expected, 2 * alpha * L, Idark * 1e9, Ilit * 1e3, Ilit / P, collected, hLow, hLowRL, hHigh, hHighRL,
              timer.elapsed());
        QVERIFY2(std::abs(lossGap / loss - 1) < 1e-6, qPrintable(QString::number(lossGap / loss - 1)));
        QVERIFY2(std::abs(Gop / expected - 1) < 0.02, qPrintable(QString::number(Gop / expected)));
        QVERIFY(std::abs(Iop / (Q * Gop) - 1) < 1e-12);
        QVERIFY(Idark > 0 && Idark < 1e-6);
        QVERIFY2(collected > 0.5 && collected < 1, qPrintable(QString::number(collected)));
        QVERIFY2(std::abs(hLow / collected - 1) < 0.01, qPrintable(QString::number(hLow / collected)));
        QVERIFY(hLowRL <= hLow * (1 + 1e-9) && hLowRL > 0.99 * hLow);
        QVERIFY2(hHigh < 0.8 * hLow, qPrintable(QString::number(hHigh / hLow)));
        QVERIFY(hHighRL <= hHigh * (1 + 1e-9));
    }

    // A germanium-silicon modulator: its direct gap 20 meV above 1550 nm's
    // photons (broadened 8 meV), reversed to -3 V - the Franz-Keldysh
    // effect's tail below the gap growing with the field. The mode's loss
    // rising tenfold and more, by what dloss_EA says (each from the medium's
    // κ, the field the semiconductor's); what is left, the edge's own
    // without a field, the same at both; its index all but still. In the
    // stripe's middle the absorption α the formula's at the field there.
    void anElectroabsorptionModulator()
    {
        QByteArray json = stripe(QStringLiteral("50[um]"), QStringLiteral(R"("material": "Germanium on silicon", "Egd": "0.82[eV]")"),
                                 QStringLiteral("0.6"), QString(),
                                         QStringLiteral(R"({"type": "study", "steps": [{"type": "sweep", "parameters": [["Va", "0, -3", "V"]]},
                {"type": "stationary", "physics": ["semi"]}, {"type": "mode", "physics": ["emw"], "frequency": "1550[nm]", "modes": 1, "neff": "3.9"}]})"));
        json.replace(R"("electroabsorption": true,)", R"("electroabsorption": true, "Ebroad": "8[meV]",)");
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(int(r.study.solutions.size()), 2);
        const Solution& off = *r.study.solutions.front();
        const Solution& on = *r.study.solutions.back();
        const double loss0 = global(off, QStringLiteral("emw.loss")), loss3 = global(on, QStringLiteral("emw.loss"));
        const double ea0 = global(off, QStringLiteral("emw.dloss_EA")), ea3 = global(on, QStringLiteral("emw.dloss_EA"));
        const double dn = global(on, QStringLiteral("emw.neff")) - global(off, QStringLiteral("emw.neff"));
        const QPointF middle(0, 0.445);
        const double F = at(on, QStringLiteral("semi.normE"), middle), alpha = at(on, QStringLiteral("emw.alphagap"), middle);
        const double photon = H * C0 / 1550e-9, formula = franzKeldysh(photon, 0.82 * Q, F, 0.03, 2e6, 0.008 * Q);
        const double L = 50e-6;
        qInfo("GeSi modulator: its loss %.4g dB/cm at 0 V, %.5g at -3 V (dloss_EA %.4g and %.5g, the edge's own %.4g and %.4g): over 50 µm %.3f dB "
              "lost, %.3f dB more at -3 V; its index moved %.2g; in its middle %.4g kV/cm, α %.5g /cm (the formula's %.5g)",
              loss0 / 100, loss3 / 100, ea0 / 100, ea3 / 100, (loss0 - ea0) / 100, (loss3 - ea3) / 100, loss0 * L, (loss3 - loss0) * L, dn, F / 1e5,
              alpha / 100, formula / 100);
        QVERIFY(loss3 > 8 * loss0);
        QVERIFY2(std::abs((loss3 - loss0) / (ea3 - ea0) - 1) < 0.01, qPrintable(QString::number((loss3 - loss0) / (ea3 - ea0))));
        QVERIFY(loss0 - ea0 > 0.05 * loss0);
        QVERIFY2(std::abs((loss3 - ea3) / (loss0 - ea0) - 1) < 0.01, qPrintable(QString::number((loss3 - ea3) / (loss0 - ea0))));
        QVERIFY(std::abs(dn) < 1e-5);
        QVERIFY2(std::abs(alpha / formula - 1) < 0.03, qPrintable(QString::number(alpha / formula)));
        QVERIFY(F > 3e6 && F < 1e7);
    }

    // The examples, solved as they come, against what they say. (A
    // sanitized build: a coarser mesh, fewer points - and nearer.)
    void thePhotodiodeExample()
    {
        QElapsedTimer timer;
        timer.start();
        Model m = example(QStringLiteral("ge_photodiode.qfem"));
        const bool slow = slowBuild();
        if (slow) only(m, QStringLiteral("std2"), QStringLiteral("frequency"), QStringLiteral("frequencies"), QStringLiteral("1[GHz], 24[GHz]"));
        Run r = run(m, QStringLiteral("std2"));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        QCOMPARE(s.snapshotFrequency(0), 1e9);
        s.select(0);   // (the first frequency, 1 GHz)
        const double I = global(s, QStringLiteral("semi.I_k")), Iop = global(s, QStringLiteral("semi.Iop"));
        const double neff = global(s, QStringLiteral("emw.neff")), loss = global(s, QStringLiteral("emw.loss"));
        const double hLow = global(s, QStringLiteral("semi.Hop_k"));
        double h24 = NAN;
        for (int k = 0; k < s.snapshotCount(); ++k) {
            s.select(k);
            if (std::abs(s.snapshotValue(k) - 24e9) < 1e8) h24 = global(s, QStringLiteral("semi.HopRL_k"));
        }
        // At 24 GHz, still above 1/√2 of its own; its 3 dB about 27.
        qInfo("Ge photodiode: neff %.4f, %.3f dB/µm; %.4g mA of q Gop %.4g (%.3f), %.4f A/W; modulated %.4f at 1 GHz, %.4f at 24 GHz "
              "through 50 Ω (%.3f of it); %lld ms",
              neff, loss * 1e-6, I * 1e3, Iop * 1e3, I / Iop, I / 1e-3, hLow, h24, h24 / hLow, timer.elapsed());
        QVERIFY(std::abs(neff - 3.84) < (slow ? 0.03 : 0.01));
        QVERIFY(std::abs(loss * 1e-6 - 1.9) < (slow ? 0.2 : 0.1));
        QVERIFY2(std::abs(I / 1e-3 - 1.15) < (slow ? 0.04 : 0.02), qPrintable(QString::number(I / 1e-3)));
        QVERIFY(std::abs(I / Iop - 0.92) < (slow ? 0.03 : 0.02));
        QVERIFY2(h24 / hLow > (slow ? 0.65 : 0.70) && h24 / hLow < 0.78, qPrintable(QString::number(h24 / hLow)));
        if (slow)
            only(m, QStringLiteral("std1"), QStringLiteral("sweep"), QStringLiteral("parameters"),
                 sweepOf(QStringLiteral("Va"), QStringLiteral("-1"), QStringLiteral("V")));
        const Run dark = run(m, QStringLiteral("std1"));
        QVERIFY2(dark.error.isEmpty(), qPrintable(dark.error));
        bool seen = false;
        for (const auto& point : dark.study.solutions)
            if (std::abs(point->sweptValues.value(QStringLiteral("Va")) + 1) < 1e-9) {
                const double Idark = global(*point, QStringLiteral("semi.I_k"));
                qInfo("its dark current at -1 V %.4g nA", Idark * 1e9);
                QVERIFY(std::abs(Idark * 1e9 - 7.2) < (slow ? 1.0 : 0.5));
                seen = true;
            }
        QVERIFY(seen);
        // The edge: at 1620 nm less than half of 1580 nm's. (Not in a
        // sanitized build: two more modes.)
        if (slow) return;
        only(m, QStringLiteral("std3"), QStringLiteral("sweep"), QStringLiteral("parameters"),
             sweepOf(QStringLiteral("lambda"), QStringLiteral("1580, 1620"), QStringLiteral("nm")));
        const Run spectral = run(m, QStringLiteral("std3"));
        QVERIFY2(spectral.error.isEmpty(), qPrintable(spectral.error));
        QCOMPARE(int(spectral.study.solutions.size()), 2);
        const double r1580 = global(*spectral.study.solutions.front(), QStringLiteral("semi.I_k")) / 1e-3;
        const double r1620 = global(*spectral.study.solutions.back(), QStringLiteral("semi.I_k")) / 1e-3;
        qInfo("its responsivity %.3f A/W at 1580 nm, %.3f at 1620", r1580, r1620);
        QVERIFY(std::abs(r1580 - 1.18) < 0.02 && std::abs(r1620 - 0.45) < 0.03);
    }

    void theModulatorExample()
    {
        QElapsedTimer timer;
        timer.start();
        Model m = example(QStringLiteral("gesi_eam.qfem"));
        const bool slow = slowBuild();
        only(m, QStringLiteral("std1"), QStringLiteral("sweep"), QStringLiteral("parameters"),
             sweepOf(QStringLiteral("Va"), QStringLiteral("0, -3"), QStringLiteral("V")));
        const Run r = run(m, QStringLiteral("std1"));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(int(r.study.solutions.size()), 2);
        const double L = 50e-6;
        const double il = global(*r.study.solutions.front(), QStringLiteral("emw.loss")) * L;
        const double on = global(*r.study.solutions.back(), QStringLiteral("emw.loss")) * L;
        const double i0 = global(*r.study.solutions.front(), QStringLiteral("semi.I_k")), i3 = global(*r.study.solutions.back(), QStringLiteral("semi.I_k"));
        qInfo("GeSi modulator: %.3f dB over 50 µm at 0 V, %.3f at -3 V (extinction %.3f dB); photocurrent %.4g µA and %.4g mA; %lld ms", il, on,
              on - il, i0 * 1e6, i3 * 1e3, timer.elapsed());
        QVERIFY(std::abs(il - 0.65) < (slow ? 0.06 : 0.03));
        QVERIFY(std::abs(on - 6.29) < (slow ? 0.3 : 0.1));
        QVERIFY(std::abs(i0 * 1e3 - 0.16) < (slow ? 0.06 : 0.01) && std::abs(i3 * 1e3 - 0.95) < (slow ? 0.04 : 0.02));
    }

private:
    /// An example as it comes - in a sanitized build, its mesh's finest
    /// sizes 0.1 µm.
    static Model example(const QString& name)
    {
        const QString file = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/") + name;
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) qFatal("no example %s", qPrintable(name));
        Model m = modelOf(f.readAll());
        m.fileName = file;
        if (slowBuild()) {
            Node* mesh = m.find(m.mesh().tag);
            for (Node& size : mesh->children) size.props.insert(QStringLiteral("hmax"), QStringLiteral("0.1"));
        }
        return m;
    }
    /// A sweep's one row, its parameter's values. (Appended: a braced list
    /// of one list is a copy to some compilers.)
    static QJsonArray sweepOf(const QString& name, const QString& values, const QString& unit)
    {
        QJsonArray rows;
        rows.append(QJsonArray{name, values, unit});
        return rows;
    }
    /// The steps of \a type in study \a study given \a value for \a key (a sweep's values, its frequencies).
    static void only(Model& m, const QString& study, const QString& type, const QString& key, const QJsonValue& value)
    {
        for (const Node* st : m.studies())
            if (st->tag == study)
                for (Node& step : m.find(st->tag)->children)
                    if (step.type == type) step.props.insert(key, value);
    }
};

QTEST_GUILESS_MAIN(TestFemOptoelectronics)
#include "test_fem_optoelectronics.moc"
