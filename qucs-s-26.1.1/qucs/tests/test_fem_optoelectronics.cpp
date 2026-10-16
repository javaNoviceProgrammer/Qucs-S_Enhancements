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
#include "fem_mesh.h"
#include <set>
#include <map>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QtTest>

#include <cmath>
#include <complex>
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
    void aPublishedLateralPhotodiode()
    {
        // Benchmark: Virot, Benedikovic et al., "Integrated waveguide PIN
        // photodiodes exploiting lateral Si/Ge/Si heterojunction", Opt.
        // Express 25, 19487 (2017): intrinsic germanium w wide, 260 nm thick
        // on a 60 nm silicon seed, between p⁺⁺ and n⁺⁺ silicon (over 1e19
        // cm⁻³) of a 220 nm SOI layer - its top 100 nm above them. Measured
        // at 1550 nm and -16 dBm: 1.16 A/W at -1 V for w 1 µm 40 µm long,
        // 0.5 for 10 µm (their FDTD 0.63: the butt coupling and a weaker
        // absorption, the paper says); its bandwidth carriers' transit
        // limited, about 18 GHz at -4 V for w 1 µm, over 50 GHz (the setup's
        // limit) at -2 V for 0.3 µm. Here, the waveguide's mode the cross
        // section's (no coupling's loss), at 25 µW:
        // - 40 µm long 1.248 A/W, between theirs and the ideal qλ/hc 1.25;
        //   10 µm long 1.231 - the mode the cross section's absorbs within 10
        //   µm; theirs, coupled from a silicon waveguide, does not;
        // - w 1 µm at -4 V 35 GHz, twice theirs (vsat 6e6 cm/s here); w 0.3
        //   µm at -2 V 96 GHz, over their 50.
        // (At 1 mW the 1 µm one's germanium filled with its own carriers,
        // 4.5e16 cm⁻³, the field screened to 2 kV/cm: 2.5 GHz.) Release
        // builds only: some 10 s there, minutes sanitized.
        if (slowBuild()) QSKIP("a benchmark: Release builds only");
        auto device = [&](const QString& w, const QString& L, const QString& va, const QString& freqs) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "parameters": [{"name": "Va", "expression": "%3"}, {"name": "P", "expression": "25[uW]"}, {"name": "lambda", "expression": "1550[nm]"},
                             {"name": "w", "expression": "%1[um]"}],
              "components": [{"unit": "um", "thickness": "%2[um]",
                "geometry": [{"type": "rectangle", "label": "box", "position": ["-2.5", "-1"], "size": ["5", "1"]},
                             {"type": "rectangle", "label": "cladding", "position": ["-2.5", "0"], "size": ["5", "1.6"]},
                             {"type": "rectangle", "label": "pplus", "position": ["-w/2 - 1", "0"], "size": ["1", "0.22"]},
                             {"type": "rectangle", "label": "nplus", "position": ["w/2", "0"], "size": ["1", "0.22"]},
                             {"type": "rectangle", "label": "seed", "position": ["-w/2", "0"], "size": ["w", "0.06"]},
                             {"type": "rectangle", "label": "ge", "position": ["-w/2", "0.06"], "size": ["w", "0.26"]},
                             {"type": "form union"}],
                "materials": [{"material": "Silicon dioxide", "selection": {"only": ["box", "cladding"]}},
                              {"material": "Silicon", "selection": {"only": ["pplus", "nplus", "seed"]}},
                              {"material": "Germanium on silicon", "selection": {"only": ["ge"]}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "selection": {"only": ["pplus", "nplus", "seed", "ge"]}, "features": [
                  {"type": "model", "saturation": true, "vsatn": "6e6[cm/s]", "vsatp": "6e6[cm/s]", "selection": {"only": ["ge"]}},
                  {"type": "model", "mobility": "doping", "saturation": true, "selection": {"only": ["pplus", "nplus", "seed"]}},
                  {"type": "doping", "dopant": "acceptor", "N0": "1e19[1/cm^3]", "selection": {"only": ["pplus"]}},
                  {"type": "doping", "dopant": "donor", "N0": "1e19[1/cm^3]", "selection": {"only": ["nplus"]}},
                  {"type": "doping", "dopant": "acceptor", "N0": "1e15[1/cm^3]", "selection": {"only": ["seed", "ge"]}},
                  {"type": "srh", "selection": {"all": true}},
                  {"type": "optical", "mode": "emw", "P0": "P", "selection": {"only": ["ge"]}},
                  {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["pplus"], "side": "left"}},
                  {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["nplus"], "side": "right"}}]},
                 {"type": "modes", "tag": "emw", "features": [
                  {"type": "wave", "electroabsorption": true, "selection": {"only": ["ge"]}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.25", "features": [
                  {"type": "size", "level": "domain", "selection": {"only": ["pplus", "nplus", "seed", "ge"]}, "hmax": "0.03"}]}}],
              "studies": [{"type": "study", "steps": [{"type": "stationary", "physics": ["semi"]},
                {"type": "mode", "physics": ["emw"], "frequency": "lambda", "modes": 1, "neff": "4"},
                {"type": "stationary", "physics": ["semi"]}, {"type": "frequency", "physics": ["semi"], "frequencies": "%4"}]}]})json")
                .arg(w, L, va, freqs)
                .toUtf8();
        };
        // The 3 dB frequency of HopRL_k against its lowest frequency's (log-interpolated).
        auto bandwidth = [&](Solution& s) -> double {
            s.select(0);
            const double low = std::abs(global(s, QStringLiteral("semi.HopRL_k")));
            double fPrev = s.snapshotValue(0), hPrev = 1;
            for (int k = 1; k < s.snapshotCount(); ++k) {
                s.select(k);
                const double f = s.snapshotValue(k), h = std::abs(global(s, QStringLiteral("semi.HopRL_k"))) / low;
                if (h < 1 / std::sqrt(2.0)) {
                    const double t = (std::log(hPrev) - std::log(1 / std::sqrt(2.0))) / (std::log(hPrev) - std::log(h));
                    return std::exp(std::log(fPrev) + t * (std::log(f) - std::log(fPrev)));
                }
                fPrev = f;
                hPrev = h;
            }
            return NAN;
        };
        QStringList said;
        struct Case {
            const char *w, *L, *va;
            bool bw;
        };
        double R40 = NAN, R10 = NAN, f1 = NAN, f03 = NAN;
        for (const Case& c : {Case{"1", "40", "-1[V]", false}, Case{"1", "10", "-1[V]", false}, Case{"1", "10", "-4[V]", true}, Case{"0.3", "10", "-2[V]", true}}) {
            const QString freqs = c.bw ? QStringLiteral("1[GHz], 5[GHz], 10[GHz], 15[GHz], 20[GHz], 25[GHz], 30[GHz], 40[GHz], 50[GHz], 60[GHz], 80[GHz], 100[GHz]") : QStringLiteral("1[GHz]");
            QElapsedTimer timer;
            timer.start();
            Run r = run(modelOf(device(QLatin1String(c.w), QLatin1String(c.L), QLatin1String(c.va), freqs)));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            Solution& s = *r.solution;
            s.select(0);
            const double R = std::abs(global(s, QStringLiteral("semi.I_k"))) / 25e-6;
            const double f3 = c.bw ? bandwidth(s) : NAN;
            said << QStringLiteral("w %1 µm, %2 µm long, %3: %4 A/W%5 (%6 ms)")
                        .arg(QLatin1String(c.w), QLatin1String(c.L), QLatin1String(c.va))
                        .arg(R, 0, 'f', 3)
                        .arg(c.bw ? QStringLiteral(", 3 dB at %1 GHz").arg(f3 / 1e9, 0, 'f', 1) : QString())
                        .arg(timer.elapsed());
            if (!c.bw && QLatin1String(c.L) == QLatin1String("40")) R40 = R;
            if (!c.bw && QLatin1String(c.L) == QLatin1String("10")) R10 = R;
            if (c.bw && QLatin1String(c.w) == QLatin1String("1")) f1 = f3;
            if (c.bw && QLatin1String(c.w) == QLatin1String("0.3")) f03 = f3;
        }
        qInfo("Virot et al.'s lateral Si/Ge/Si photodiode: %s", qPrintable(said.join(QLatin1String("; "))));
        QVERIFY2(R40 > 1.15 && R40 < 1.26 && R10 > 1.15, qPrintable(said.join(QLatin1String("; "))));
        QVERIFY2(f1 > 18e9 && f1 < 45e9 && f03 > 50e9, qPrintable(said.join(QLatin1String("; "))));
    }

    // Benchmark: the GeSi electroabsorption modulator of Liu et al. (Nat.
    // Photonics 2, 433, 2008) as Bernardis's thesis (MIT, 2008) gives it:
    // germanium with 0.75 % silicon, 0.4 µm high, 0.6 µm wide, 50 µm long -
    // 6.5 dB of extinction at 1550 nm at an effective -2.5 V, its extinction
    // some 2.2 dB a volt up to 5.5 dB, 5.2 dB or more from 1510 to 1555 nm.
    // Here the modulator example 0.4 µm high, its direct gap the one thing
    // not given. Tensile germanium's 0.773 eV raised 3.6 eV a unit of
    // silicon (the example's 1.4 % at 0.82 eV) puts it at 0.80 eV - on
    // 1550 nm's photons, 18.5 dB lost at 0 V: no modulator. 10 meV higher
    // the measured extinction at 1550 nm comes out, its slope near the
    // measured one; but its window is narrower - the field lowers the loss
    // above the gap and below 1540 nm there is little to modulate.
    void aPublishedElectroabsorptionModulator()
    {
        if (slowBuild()) QSKIP("a benchmark: Release builds only");
        const QString file = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/gesi_eam.qfem");
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray text = f.readAll();
        QCOMPARE(text.count(R"("0.45")"), 3);
        QCOMPARE(text.count(R"("Egd": "0.82[eV]")"), 1);
        QCOMPARE(text.count(R"("expression": "1550[nm]")"), 1);
        // Over 50 µm, in dB, at each bias.
        const auto losses = [&](const QString& gap, const QString& nm, const QString& biases) -> std::vector<double> {
            QByteArray json = text;
            json.replace(R"("0.45")", R"("0.4")");
            json.replace(R"("Egd": "0.82[eV]")", QStringLiteral(R"("Egd": "%1[eV]")").arg(gap).toUtf8());
            json.replace(R"("expression": "1550[nm]")", QStringLiteral(R"("expression": "%1[nm]")").arg(nm).toUtf8());
            Model m = modelOf(json);
            only(m, QStringLiteral("std1"), QStringLiteral("sweep"), QStringLiteral("parameters"), sweepOf(QStringLiteral("Va"), biases, QStringLiteral("V")));
            const Run r = run(m, QStringLiteral("std1"));
            if (!r.error.isEmpty()) qWarning("%s", qPrintable(r.error));
            std::vector<double> out;
            for (const auto& point : r.study.solutions) out.push_back(global(*point, QStringLiteral("emw.loss")) * 50e-6);
            return out;
        };
        const std::vector<double> law = losses(QStringLiteral("0.80"), QStringLiteral("1550"), QStringLiteral("0, -2.5"));
        const std::vector<double> at1550 = losses(QStringLiteral("0.81"), QStringLiteral("1550"), QStringLiteral("0, -0.5, -1, -1.5, -2, -2.5"));
        const std::vector<double> at1530 = losses(QStringLiteral("0.81"), QStringLiteral("1530"), QStringLiteral("0, -2.5"));
        const std::vector<double> at1555 = losses(QStringLiteral("0.81"), QStringLiteral("1555"), QStringLiteral("0, -2.5"));
        QVERIFY(law.size() == 2 && at1550.size() == 6 && at1530.size() == 2 && at1555.size() == 2);
        // The extinction's slope to -2 V (up to 5.5 dB), a line through the origin.
        double xy = 0, xx = 0;
        for (int k = 1; k <= 4; ++k) xy += 0.5 * k * (at1550[k] - at1550[0]), xx += 0.25 * k * k;
        const double slope = xy / xx, extinction = at1550[5] - at1550[0];
        qInfo("Liu et al.'s GeSi modulator, 50 µm: at 0.80 eV %.1f dB lost at 0 V, %.2f dB of extinction at -2.5 V; at 0.81 eV %.2f dB lost, "
              "%.2f dB of extinction at 1550 nm (6.5 measured), %.2f dB a volt (2.2), %.2f dB at 1530 nm and %.2f at 1555 (5.2 or more measured)",
              law[0], law[1] - law[0], at1550[0], extinction, slope, at1530[1] - at1530[0], at1555[1] - at1555[0]);
        QVERIFY2(law[0] > 10 && law[1] - law[0] < 4, qPrintable(QString::number(law[0])));
        QVERIFY2(std::abs(extinction - 6.5) < 0.5, qPrintable(QString::number(extinction)));
        QVERIFY2(slope > 2.2 && slope < 3.2, qPrintable(QString::number(slope)));
        QVERIFY(at1550[0] < 5 && at1555[1] - at1555[0] > 5);
        QVERIFY2(at1530[1] - at1530[0] < 3.5, qPrintable(QString::number(at1530[1] - at1530[0])));
    }

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
        // (Each pair a photon lost across the gap: all the mode loses here.)
        QVERIFY2(std::abs(Gop / expected - 1) < 1e-6, qPrintable(QString::number(Gop / expected - 1)));
        QVERIFY(std::abs(Iop / (Q * Gop) - 1) < 1e-12);
        QVERIFY(Idark > 0 && Idark < 1e-6);
        QVERIFY2(collected > 0.5 && collected < 1, qPrintable(QString::number(collected)));
        QVERIFY2(std::abs(hLow / collected - 1) < 0.01, qPrintable(QString::number(hLow / collected)));
        QVERIFY(hLowRL <= hLow * (1 + 1e-9) && hLowRL > 0.99 * hLow);
        QVERIFY2(hHigh < 0.8 * hLow, qPrintable(QString::number(hHigh / hLow)));
        QVERIFY(hHighRL <= hHigh * (1 + 1e-9));
    }

    // A material whose κ is its gap's edge (Germanium on silicon's: its
    // Egd 0.773 eV and A 2e4 /cm, as the Direct-gap absorption takes them)
    // and that option on: the option's absorption in place of the
    // material's κ, not added to it - at no field the mode loses what it
    // loses by the material's κ alone (it lost twice that). The material's
    // κ given as nought: the same again.
    void theGapsEdgeCountedOnce()
    {
        auto lossOf = [](const QString& wave, const QString& kappa) {
            const QString hmax = slowBuild() ? QStringLiteral("0.15") : QStringLiteral("0.08");
            const QByteArray json = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "label": "clad", "position": ["-2", "-1.5"], "size": ["4", "3"]},
                             {"type": "rectangle", "label": "core", "position": ["-0.3", "-0.2"], "size": ["0.6", "0.4"]}],
                "materials": [{"material": "Silicon dioxide", "selection": {"only": ["clad"]}},
                              {"material": "Germanium on silicon", %2 "selection": {"only": ["core"]}}],
                "physics": [{"type": "modes", "tag": "emw", %3 "features": [{"type": "wave", %1 "selection": {"only": ["core"]}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "%4"}}],
              "studies": [{"type": "study", "steps": [{"type": "mode", "physics": ["emw"], "frequency": "1550[nm]", "modes": 1, "neff": "3.8"}]}]})json")
                                        .arg(wave, kappa, slowBuild() ? QStringLiteral(R"("order": "1",)") : QString(), hmax)
                                        .toUtf8();
            const Run r = run(modelOf(json));
            if (!r.error.isEmpty()) qWarning("%s", qPrintable(r.error));
            return r.error.isEmpty() ? global(*r.solution, QStringLiteral("emw.alpha")) : NAN;
        };
        const double byKappa = lossOf(QString(), QString());
        const double byOption = lossOf(QStringLiteral(R"("electroabsorption": true, "Fea": "0[V/m]",)"), QString());
        const double noughtKappa = lossOf(QStringLiteral(R"("electroabsorption": true, "Fea": "0[V/m]",)"), QStringLiteral(R"("kappa": "0",)"));
        const double none = lossOf(QString(), QStringLiteral(R"("kappa": "0",)"));
        qInfo("Ge on Si at 1550 nm: its mode's α %.6g /cm by its κ, %.6g by the option (its κ nought: %.6g; neither: %.3g)", byKappa / 100,
              byOption / 100, noughtKappa / 100, none / 100);
        QVERIFY2(byKappa > 1000 && std::abs(none) < 1e-6 * byKappa, qPrintable(QStringLiteral("%1 %2").arg(byKappa).arg(none)));
        QVERIFY2(std::abs(byOption / byKappa - 1) < 1e-6, qPrintable(QString::number(byOption / byKappa)));
        QVERIFY2(std::abs(noughtKappa / byOption - 1) < 1e-9, qPrintable(QString::number(noughtKappa / byOption)));
    }

    // The light from the mode asked for: a mode step finding three, the
    // photodiode lit by mode 1 (the highest index), then by mode 3 - the
    // step leaving it selected (it left the last lighting it whatever was
    // asked), its pairs from that mode's own loss across the gap, never more
    // than its photons; mode 4 of three refused.
    void theModeThatLightsIt()
    {
        const auto lit = [](int number) {
            const QString light = QStringLiteral(R"({"type": "optical", "mode": "emw", "number": %1, "P0": "P", "selection": {"only": ["pplus", "absorber", "nplus"]}},)")
                                      .arg(number);
            return run(modelOf(stripe(QStringLiteral("15[um]"), QStringLiteral(R"("material": "Germanium on silicon")"), QStringLiteral("0.7"), light,
                                      QStringLiteral(R"({"type": "study", "steps": [{"type": "stationary", "physics": ["semi"]},
                {"type": "mode", "physics": ["emw"], "frequency": "1550[nm]", "modes": 3, "neff": "3.9"},
                {"type": "stationary", "physics": ["semi"]}]})"))));
        };
        const double L = 15e-6, P = 1e-3, photon = H * C0 / 1550e-9;
        double neff[2] = {0, 0};
        for (int k : {1, 3}) {
            const Run r = lit(k);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.solution;
            QCOMPARE(global(s, QStringLiteral("emw.mode")), double(k));
            const double alpha = global(s, QStringLiteral("emw.alpha")), loss = global(s, QStringLiteral("emw.loss"));
            const double lossGap = global(s, QStringLiteral("emw.loss_gap")), Gop = global(s, QStringLiteral("semi.Gop"));
            const double expected = P * -std::expm1(-2 * alpha * L) / photon * lossGap / loss;
            neff[k == 1 ? 0 : 1] = global(s, QStringLiteral("emw.neff"));
            qInfo("lit by mode %d: neff %.5f, %.1f dB/cm (%.1f across the gap); pairs %.6g /s (its photons lost across the gap %.6g); %.4f A/W", k,
                  global(s, QStringLiteral("emw.neff")), loss / 100, lossGap / 100, Gop, expected, global(s, QStringLiteral("semi.I_k")) / P);
            QVERIFY2(std::abs(Gop / expected - 1) < 1e-6, qPrintable(QString::number(Gop / expected - 1)));
            QVERIFY(Gop <= P * -std::expm1(-2 * alpha * L) / photon * (1 + 1e-9));
        }
        QVERIFY(neff[0] > neff[1]);
        const Run beyond = lit(4);
        QVERIFY(!beyond.error.isEmpty());
        QVERIFY2(beyond.error.contains(QLatin1String("mode 4")), qPrintable(beyond.error));
    }

    // A wavelength sweep's mode followed: each point searching about the
    // mode the point before found, not about the index the step names - the
    // photodiode example's spectrum, 1500 to 1640 nm by 20, its effective
    // index moving by less than 0.03 a step (germanium's measured index has
    // its peak at the direct gap: it rises, then falls); searched about the
    // step's index each, at 1600 nm it jumps to another branch.
    void aSweepFollowsItsMode()
    {
        // (A sanitized build: 1560 to 1600 nm, its mode solves slow - and
        // followed only.)
        const bool slow = slowBuild();
        const auto spectrum = [slow](bool follow) {
            Model m = example(QStringLiteral("ge_photodiode.qfem"));
            only(m, QStringLiteral("std3"), QStringLiteral("sweep"), QStringLiteral("parameters"),
                 sweepOf(QStringLiteral("lambda"), slow ? QStringLiteral("1560, 1580, 1600") : QStringLiteral("range(1500, 20, 1640)"), QStringLiteral("nm")));
            only(m, QStringLiteral("std3"), QStringLiteral("mode"), QStringLiteral("follow"), follow);
            const Run r = run(m, QStringLiteral("std3"));
            std::vector<double> neff;
            if (!r.error.isEmpty()) qWarning("%s", qPrintable(r.error));
            for (const auto& point : r.study.solutions) neff.push_back(global(*point, QStringLiteral("emw.neff")));
            return neff;
        };
        const std::vector<double> followed = spectrum(true), not_ = slow ? std::vector<double>() : spectrum(false);
        QStringList a, b;
        for (double v : followed) a << QString::number(v, 'f', 4);
        for (double v : not_) b << QString::number(v, 'f', 4);
        qInfo("followed: %s; about 3.9 each: %s", qPrintable(a.join(' ')), qPrintable(b.join(' ')));
        QCOMPARE(int(followed.size()), slow ? 3 : 8);
        for (std::size_t k = 1; k < followed.size(); ++k) QVERIFY2(std::abs(followed[k] - followed[k - 1]) < 0.03, qPrintable(a.join(' ')));
        if (!slow) {
            QVERIFY2(followed.back() < followed.front() - 0.05, qPrintable(a.join(' ')));
            double jump = 0;
            for (std::size_t k = 1; k < not_.size(); ++k) jump = std::max(jump, std::abs(not_[k] - not_[k - 1]));
            QVERIFY2(jump > 0.06, qPrintable(b.join(' ')));
        }
    }

    // A semiconductor's mesh Delaunay: every edge in its domains weighed
    // 0 or more by the box method (two angles opposite π at most, a
    // segment's angle on their side 90° at most) - the smoothing had left
    // 9 edges of the photodiode example's not, two segments had an obtuse
    // angle across them: their currents left out, said in the log. A few
    // triangles more. Solved, nothing said.
    void aSemiconductorsMeshIsDelaunay()
    {
        for (const char* name : {"ge_photodiode.qfem", "gesi_eam.qfem"}) {
            const Model m = example(QString::fromLatin1(name));
            const auto ps = evaluateParameters(m);
            const GeometryBuild g = buildGeometry(m, *ps);
            QVERIFY(g.ok());
            QString why;
            const MeshSizes sizes = meshSizes(m, *ps, *g.topology, &why);
            int flagged = 0;
            for (char f : sizes.delaunay) flagged += f;
            QCOMPARE(flagged, 4);   // (the semiconductor physics: slab, pplus, absorber, nplus - not the oxide)
            const MeshBuild b = buildMesh(g.topology, sizes);
            QVERIFY2(b.mesh, qPrintable(b.error));
            const Mesh& mesh = *b.mesh;
            std::map<std::pair<int, int>, double> weight;   // edge: Σ ½ cot of the angles opposite, in flagged triangles
            std::set<std::pair<int, int>> inFlagged;
            for (int t = 0; t < int(mesh.triangles.size()); ++t) {
                if (!sizes.delaunay[std::size_t(mesh.domain[std::size_t(t)])]) continue;
                const auto& v = mesh.triangles[std::size_t(t)];
                for (int k = 0; k < 3; ++k) {
                    const int a = v[std::size_t(k)], c = v[std::size_t((k + 1) % 3)], o = v[std::size_t((k + 2) % 3)];
                    const QPointF u = mesh.nodes[std::size_t(a)] - mesh.nodes[std::size_t(o)], w = mesh.nodes[std::size_t(c)] - mesh.nodes[std::size_t(o)];
                    weight[{std::min(a, c), std::max(a, c)}] += 0.5 * (u.x() * w.x() + u.y() * w.y()) / std::abs(u.x() * w.y() - u.y() * w.x());
                }
            }
            int bad = 0;
            for (const auto& [edge, w] : weight)
                if (w < -1e-12) ++bad;
            qInfo("%s: %d triangles, %d edges in its semiconductors, %d of them weighed below 0", name, int(mesh.triangles.size()), int(weight.size()), bad);
            QCOMPARE(bad, 0);
            // Each segment's triangles where they are.
            for (const Mesh::Edge& e : mesh.edges)
                for (int t : {e.left, e.right}) {
                    if (t < 0) continue;
                    const auto& v = mesh.triangles[std::size_t(t)];
                    QVERIFY(std::count(v.begin(), v.end(), e.a) == 1 && std::count(v.begin(), v.end(), e.b) == 1);
                }
        }
        // Solved: no edge left out.
        Model m = example(QStringLiteral("ge_photodiode.qfem"));
        const Run r = run(m, QStringLiteral("std1"));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        for (const auto& point : r.study.solutions)
            for (const QString& line : point->log) QVERIFY2(!line.contains(QLatin1String("not Delaunay")), qPrintable(line));
    }

    // A photodiode's light switched on (Optical Generation's power of t:
    // 1 µW in half a picosecond, small enough to be linear): its photocurrent
    // in time rises to what it is at rest lit (its share of q Gop collected
    // as the frequency step's at 1 MHz), and its step response, transformed
    // - H(ω) = S + jω ∫ (s(t) - S) e^(-jωt) dt -, is the frequency step's
    // response at 10 and 20 GHz.
    void aPhotodiodesStepResponse()
    {
        const bool slow = slowBuild();
        auto model = [](const QString& power, const QString& last) {
            const QString light = QStringLiteral(R"({"type": "optical", "mode": "emw", "P0": "%1", "selection": {"only": ["pplus", "absorber", "nplus"]}},)").arg(power);
            return modelOf(stripe(QStringLiteral("15[um]"), QStringLiteral(R"("material": "Germanium on silicon")"), QStringLiteral("0.7"), light,
                                  QStringLiteral(R"x({"type": "study", "steps": [{"type": "stationary", "physics": ["semi"]},
                {"type": "mode", "physics": ["emw"], "frequency": "1550[nm]", "modes": 1, "neff": "3.9"}, %1]})x").arg(last)));
        };
        // In time: outputs every quarter picosecond, then wider (the tail).
        const QString times = slow ? QStringLiteral("range(0, 1[ps], 16[ps]), range(18[ps], 2[ps], 40[ps]), range(45[ps], 5[ps], 100[ps])")
                                   : QStringLiteral("range(0, 0.25[ps], 20[ps]), range(20.5[ps], 0.5[ps], 60[ps]), range(61[ps], 1[ps], 150[ps]), range(155[ps], 5[ps], 300[ps])");
        Run timed = run(model(QStringLiteral("1[uW]*min(t/0.5[ps], 1)"),
                              QStringLiteral(R"x({"type": "transient", "physics": ["semi"], "times": "%1", "rtol": "%2"})x").arg(times, slow ? QStringLiteral("1e-3") : QStringLiteral("1e-5"))));
        QVERIFY2(timed.error.isEmpty(), qPrintable(timed.error));
        Solution& s = *timed.solution;
        // (Its times a series of their own, not after the mode's snapshot.)
        QVERIFY(s.timeDependent && !s.modeAnalysis && !s.frequencyDomain);
        QCOMPARE(s.snapshotTime(0), 0.0);
        QVERIFY2(s.snapshotText(0).startsWith(QLatin1String("t = ")), qPrintable(s.snapshotText(0)));
        const int last = s.snapshotCount() - 1;
        s.select(0);
        const double dark = global(s, QStringLiteral("semi.I_k"));
        s.select(last);
        const double S = global(s, QStringLiteral("semi.I_k")) - dark, qGop = global(s, QStringLiteral("semi.Iop"));
        std::vector<double> t, y;
        for (int k = 0; k <= last; ++k) {
            s.select(k);
            t.push_back(s.snapshotTime(k));
            y.push_back((global(s, QStringLiteral("semi.I_k")) - dark) / S - 1);
        }
        // At rest lit, and in frequency.
        Run fixed = run(model(QStringLiteral("1[uW]"), QStringLiteral(R"x({"type": "stationary", "physics": ["semi"]},
                {"type": "frequency", "physics": ["semi"], "frequencies": "1[MHz], 10[GHz], 20[GHz]"})x")));
        QVERIFY2(fixed.error.isEmpty(), qPrintable(fixed.error));
        Solution& f = *fixed.solution;
        f.select(0);
        const double rest = global(f, QStringLiteral("semi.I_k")) - dark, hLow = global(f, QStringLiteral("semi.Hop_k"));
        QStringList said;
        double worst = 0;
        for (int k : {1, 2}) {
            f.select(k);
            const double hop = global(f, QStringLiteral("semi.Hop_k")) / hLow, w = 2 * M_PI * f.snapshotFrequency(k);
            std::complex<double> integral = 0;
            for (std::size_t i = 1; i < t.size(); ++i) {
                const auto g = [&](std::size_t j) { return y[j] * std::exp(std::complex<double>(0, -w * t[j])); };
                integral += 0.5 * (g(i) + g(i - 1)) * (t[i] - t[i - 1]);
            }
            const double h = std::abs(1.0 + std::complex<double>(0, w) * integral);
            said << QStringLiteral("%1 GHz: from the step %2, the frequency step's %3").arg(f.snapshotFrequency(k) / 1e9).arg(h, 0, 'f', 4).arg(hop, 0, 'f', 4);
            worst = std::max(worst, std::abs(h / hop - 1));
        }
        qInfo("step response: settled at %.5g A (at rest lit %.5g; %.4f of q Gop, the frequency step's %.4f at 1 MHz); %s", S, rest, S / qGop, hLow,
              qPrintable(said.join(QLatin1String(", "))));
        QVERIFY2(std::abs(S / rest - 1) < 1e-3, qPrintable(QString::number(S / rest)));
        QVERIFY2(std::abs(S / qGop / hLow - 1) < 1e-3, qPrintable(QString::number(S / qGop / hLow)));
        QVERIFY2(worst < (slow ? 0.03 : 0.005), qPrintable(said.join(QLatin1String(", "))));
        // (A mode analysis before a step in time is its light; after one, or
        // a step in frequency with one, a study has times or frequencies.)
        for (const char* steps : {R"x({"type": "transient", "physics": ["semi"], "times": "range(0, 1[ps], 2[ps])"}, {"type": "mode", "physics": ["emw"], "frequency": "1550[nm]", "modes": 1})x",
                                  R"x({"type": "transient", "physics": ["semi"], "times": "range(0, 1[ps], 2[ps])"}, {"type": "frequency", "physics": ["semi"], "frequencies": "1[GHz]"})x"}) {
            const Run r = run(model(QStringLiteral("1[uW]"), QString::fromLatin1(steps)));
            QVERIFY2(r.error.contains(QLatin1String("in time and in frequency")), qPrintable(r.error));
        }
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
    // sanitized build: a coarser mesh, fewer points - and wider bounds. On
    // its 0.1 µm the figures move with the mesh, whose points differ from
    // one platform to another: between 0.08 and 0.12 µm the modulator's loss
    // at 0 V 0.602 to 0.610 dB, its photocurrents 157 to 159 µA and 0.944 to
    // 0.947 mA, the photodiode's 1.148 to 1.156 A/W and 7.3 to 7.5 nA dark -
    // the extinction 5.657 to 5.668 dB. While a node between germanium and
    // silicon took its first triangle's medium, the loss 0.62 to 0.77 dB, the
    // photocurrents 160 to 189 µA, Linux's and macOS's alike.)
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
        // Its Derived Values as the example has them: R = semi.I_k/semi.Pin
        // in A/W (a current over a power - its dimension's 1/V, its SI form
        // s³·A/(m²·kg)); a sum, a product, a number over a power not named so.
        {
            const Node* derived = nullptr;
            std::function<void(const Node&)> find = [&](const Node& n) {
                if (n.type == QLatin1String("global") && n.text(QStringLiteral("expression")).contains(QLatin1String("semi.Pin"))) derived = &n;
                for (const Node& c : n.children) find(c);
            };
            find(m.root);
            QVERIFY(derived != nullptr);
            const DerivedResult d = evaluateDerived(s, *derived);
            QStringList shown;
            for (const DerivedValue& v : d.values) shown << v.name + QStringLiteral(" = ") + v.text;
            qInfo("its Derived Values: %s", qPrintable(shown.join(QLatin1String("; "))));
            bool seen = false;
            for (const DerivedValue& v : d.values)
                if (v.name == QLatin1String("R")) seen = v.text.endsWith(QLatin1String(" A/W"));
            QVERIFY2(seen, qPrintable(shown.join(QLatin1String("; "))));
            Node other = *derived;
            other.props.insert(QStringLiteral("expression"), QStringLiteral("semi.I_k/semi.Pin*2; 1/semi.Pin; semi.I_k/(semi.Pin*1)"));
            const DerivedResult e = evaluateDerived(s, other);
            QCOMPARE(int(e.values.size()), 3);
            QVERIFY2(!e.values[0].text.contains(QLatin1String("A/W")) && !e.values[1].text.contains(QLatin1String("/W"))
                         && e.values[2].text.endsWith(QLatin1String(" A/W")),
                     qPrintable(e.values[0].text + QStringLiteral(" | ") + e.values[1].text + QStringLiteral(" | ") + e.values[2].text));
        }
        const double I = global(s, QStringLiteral("semi.I_k")), Iop = global(s, QStringLiteral("semi.Iop"));
        const double neff = global(s, QStringLiteral("emw.neff")), loss = global(s, QStringLiteral("emw.loss"));
        const double hLow = global(s, QStringLiteral("semi.Hop_k"));
        double h24 = NAN;
        for (int k = 0; k < s.snapshotCount(); ++k) {
            s.select(k);
            if (std::abs(s.snapshotValue(k) - 24e9) < 1e8) h24 = global(s, QStringLiteral("semi.HopRL_k"));
        }
        // At 24 GHz, still above 1/√2 of its own (0.771); its 3 dB about 29
        // (27 while saturation was driven by the whole field: the doped
        // germanium's carriers, diffusing out, slowed by the heterojunction's
        // field across them).
        qInfo("Ge photodiode: neff %.4f, %.3f dB/µm; %.4g mA of q Gop %.4g (%.3f), %.4f A/W; modulated %.4f at 1 GHz, %.4f at 24 GHz "
              "through 50 Ω (%.3f of it); %lld ms",
              neff, loss * 1e-6, I * 1e3, Iop * 1e3, I / Iop, I / 1e-3, hLow, h24, h24 / hLow, timer.elapsed());
        // (3.84 while germanium's index was 4.275, a constant; Nunley et al.'s
        // moved 27 meV with the strained gap, 4.241 at 1550 nm, moves the
        // mode to 3.81 - the example's guess 3.82, its next mode up 3.94.)
        QVERIFY2(std::abs(neff - 3.81) < (slow ? 0.03 : 0.01), qPrintable(QString::number(neff)));
        QVERIFY(std::abs(loss * 1e-6 - 1.9) < (slow ? 0.2 : 0.1));
        QVERIFY2(std::abs(I / 1e-3 - 1.155) < (slow ? 0.06 : 0.02), qPrintable(QString::number(I / 1e-3)));
        QVERIFY(std::abs(I / Iop - 0.925) < (slow ? 0.03 : 0.02));
        QVERIFY2(h24 / hLow > (slow ? 0.65 : 0.70) && h24 / hLow < 0.80, qPrintable(QString::number(h24 / hLow)));
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
                QVERIFY(std::abs(Idark * 1e9 - 7.4) < (slow ? 1.5 : 0.5));
                seen = true;
            }
        QVERIFY(seen);
        // Its spectrum as the example sweeps it, 1500 to 1640 nm by 20 - the
        // mode followed from 1500 nm -, the strained edge's fall past 1580.
        // (Not in a sanitized build: eight mode solves.)
        if (slow) return;
        const Run spectral = run(m, QStringLiteral("std3"));
        QVERIFY2(spectral.error.isEmpty(), qPrintable(spectral.error));
        QCOMPARE(int(spectral.study.solutions.size()), 8);
        std::vector<double> R, index;
        for (const auto& point : spectral.study.solutions) {
            R.push_back(global(*point, QStringLiteral("semi.I_k")) / 1e-3);
            index.push_back(global(*point, QStringLiteral("emw.neff")));
        }
        QStringList said;
        for (std::size_t k = 0; k < R.size(); ++k) said << QStringLiteral("%1 nm %2 A/W (neff %3)").arg(1500 + 20 * k).arg(R[k], 0, 'f', 3).arg(index[k], 0, 'f', 4);
        qInfo("its spectrum: %s", qPrintable(said.join(QLatin1String(", "))));
        // One mode throughout (its index moving by less than 0.03 a step);
        // the most at 1560 nm, then the edge: 1580 its 1.09, 1620 less than
        // half of it, as the example says.
        for (std::size_t k = 1; k < index.size(); ++k) QVERIFY2(std::abs(index[k] - index[k - 1]) < 0.03, qPrintable(said.join(' ')));
        QCOMPARE(int(std::max_element(R.begin(), R.end()) - R.begin()), 3);
        // (1580 and 1600 nm 1.12 and 0.905 while germanium's index was a
        // constant: its measured one moves the mode.)
        QVERIFY2(std::abs(R[4] - 1.094) < 0.02 && std::abs(R[5] - 0.879) < 0.02 && std::abs(R[6] - 0.41) < 0.02 && std::abs(R[7] - 0.16) < 0.02,
                 qPrintable(said.join(' ')));
        QVERIFY(R[6] < 0.5 * R[4]);
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
        QVERIFY(std::abs(il - 0.615) < (slow ? 0.2 : 0.03));
        QVERIFY(std::abs(on - 6.26) < (slow ? 0.3 : 0.1));
        // (5.65 while germanium's index was a constant 4.275.)
        QVERIFY2(std::abs(on - il - 5.63) < (slow ? 0.05 : 0.02), qPrintable(QString::number(on - il)));
        QVERIFY(std::abs(i0 * 1e3 - 0.160) < (slow ? 0.09 : 0.01) && std::abs(i3 * 1e3 - 0.95) < (slow ? 0.1 : 0.02));
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
