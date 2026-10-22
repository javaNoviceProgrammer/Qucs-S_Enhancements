/*
 * test_fem_mobility.cpp - the semiconductor's mobility models: Arora's,
 *                         Dorkel and Leturcq's, Klaassen's, Yamaguchi's,
 *                         Shirahata's surface scattering, an expression of
 *                         the state, the saturation velocities' exponential
 *                         temperature model, Lombardi's at the lattice's
 *                         temperature
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

#include <QtTest>

#include <cmath>
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

double at(const Solution& s, const QString& expression, double x, double y)
{
    const Locator locator(s.mesh());
    const std::optional<double> v = evaluateAt(s, locator, expression, QPointF(x, y));
    return v ? *v : NAN;
}

/// A silicon bar along x (µm): 2 long, 0.2 high, its donors \a Nd and
/// acceptors \a Na (cm⁻³; "0" none), contact 1 at its left grounded and
/// 2 at its right at \a V; \a model the Semiconductor Material Model's
/// more (its mobility...), \a physics the physics' (its T, TL), \a
/// features more features, meshed at \a hmax.
QByteArray bar(const QString& Nd, const QString& Na, const QString& V, const QString& model, const QString& physics = QString(),
               const QString& features = QString(), const QString& hmax = QStringLiteral("0.05"))
{
    QString dopings;
    if (Nd != QLatin1String("0"))
        dopings += QStringLiteral(R"(, {"type": "doping", "dopant": "donor", "N0": "%1[1/cm^3]", "selection": {"all": true}})").arg(Nd);
    if (Na != QLatin1String("0"))
        dopings += QStringLiteral(R"(, {"type": "doping", "dopant": "acceptor", "N0": "%1[1/cm^3]", "selection": {"all": true}})").arg(Na);
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "label": "bar", "size": ["2", "0.2"]}],
        "materials": [{"material": "Silicon", "selection": {"all": true}}],
        "physics": [{"type": "semiconductor", "tag": "semi" %1, "features": [
          {"type": "model", %2 "selection": {"all": true}} %3,
          {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}},
          {"type": "ohmic", "name": "2", "V0": "%4", "selection": {"of": ["bar"], "side": "right"}} %5]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "%6"}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
        .arg(physics, model, dopings, V, features, hmax)
        .toUtf8();
}

/// A MOS capacitor (µm): p-type silicon \a Na (cm⁻³) 0.4 wide, 0.3 deep
/// below y = 0, its top 30 nm meshed at 2 nm (5 sanitized); 10 nm of oxide over it, a
/// gate on top at \a Vg (its metal's work function silicon's affinity);
/// the body contact at the bottom; \a model the material model's more,
/// \a physics the physics'. At rest: each triangle's mobility as at its
/// own field across the interface.
QByteArray mos(const QString& Na, const QString& Vg, const QString& model, const QString& physics = QString())
{
    // (A sanitized build: 5 nm, its solves some 100 s at 2 there.)
    const QString fine = slowBuild() ? QStringLiteral("0.005") : QStringLiteral("0.002");
    return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "label": "skin", "position": ["0", "-0.03"], "size": ["0.4", "0.03"]},
                     {"type": "rectangle", "label": "body", "position": ["0", "-0.3"], "size": ["0.4", "0.27"]},
                     {"type": "rectangle", "label": "oxide", "position": ["0", "0"], "size": ["0.4", "0.01"]}],
        "materials": [{"material": "Silicon", "selection": {"only": ["skin", "body"]}},
                      {"material": "Silicon dioxide", "selection": {"only": ["oxide"]}}],
        "physics": [{"type": "semiconductor", "tag": "semi" %4, "features": [
          {"type": "doping", "dopant": "acceptor", "N0": "%1[1/cm^3]", "selection": {"only": ["skin", "body"]}},
          {"type": "model", %3 "selection": {"all": true}},
          {"type": "gate", "name": "g", "V0": "%2", "phim": "4.05[eV]", "selection": {"of": ["oxide"], "side": "top"}},
          {"type": "ohmic", "name": "b", "V0": "0[V]", "selection": {"of": ["body"], "side": "bottom"}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "0.03", "features": [
          {"type": "size", "level": "domain", "selection": {"only": ["skin", "oxide"]}, "hmax": "%5"}]}}],
      "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json")
        .arg(Na, Vg, model, physics, fine)
        .toUtf8();
}

// The models as their papers give them (cm, V, s, K), the test's own.

/// Arora, Hauser and Roulston (1982): µmin Tn^-0.57 + µd Tn^b/(1 + (N/(Nref
/// Tn^2.4))^(0.88 Tn^-0.146)).
double arora(bool electrons, double N, double T)
{
    const double tn = T / 300;
    const double mumin = electrons ? 88 : 54.3, mud = electrons ? 1252 : 407, b = electrons ? -2.33 : -2.23, nref = electrons ? 1.26e17 : 2.35e17;
    return mumin * std::pow(tn, -0.57) + mud * std::pow(tn, b) / (1 + std::pow(N / (nref * std::pow(tn, 2.4)), 0.88 * std::pow(tn, -0.146)));
}

/// Masetti, Severi and Solmi (1983): silicon's arsenic (electrons) and
/// boron (holes) - their µmax 1417 and 470.5 at 300 K, (T/300 K)^-ζ at \a T
/// (ζ 2.5 and 2.2), else \a mumax.
double masetti(bool electrons, double N, double T = 300, double mumax = 0)
{
    const double top = mumax > 0 ? mumax : (electrons ? 1417 : 470.5) * std::pow(T / 300, electrons ? -2.5 : -2.2);
    if (electrons) return 52.2 + (top - 52.2) / (1 + std::pow(N / 9.68e16, 0.68)) - 43.4 / (1 + std::pow(3.43e20 / N, 2));
    return 44.9 * std::exp(-9.23e16 / N) + top / (1 + std::pow(N / 2.23e17, 0.719)) - 29.0 / (1 + std::pow(6.1e20 / N, 2));
}

/// Dorkel and Leturcq (1981).
double dorkel(bool electrons, double N, double n, double p, double T)
{
    const double lattice = (electrons ? 1430 : 495) * std::pow(T / 300, -2.2);
    const double A = electrons ? 4.61e17 : 1e17, B = electrons ? 1.52e15 : 6.25e14;
    const double bt = B * T * T / N;
    const double muI = A * std::pow(T, 1.5) / N / (std::log(1 + bt) - bt / (1 + bt));
    const double muCC = 2e17 * std::pow(T, 1.5) / std::sqrt(n * p) / std::log(1 + 8.28e8 * T * T / std::cbrt(n * p));
    const double X = std::sqrt(6 * lattice * (muI + muCC) / (muI * muCC));
    return lattice * (1.025 / (1 + std::pow(X / 1.68, 1.43)) - 0.025);
}

/// Klaassen (1992), as device simulators take it (r5 -0.8552, G's
/// minimum below its P): \a electrons', at donors \a Nd, acceptors \a Na
/// and carriers \a n, \a p.
double klaassen(bool electrons, double Nd, double Na, double n, double p, double T)
{
    const double tn = T / 300, me = 1, mh = 1.258;
    const double mumax = electrons ? 1417 : 470.5, mumin = electrons ? 52.2 : 44.9, theta = electrons ? 2.285 : 2.247;
    const double nref = electrons ? 9.68e16 : 2.23e17, alpha = electrons ? 0.68 : 0.719;
    auto cluster = [](double N, double c, double r) { return N > 0 ? N * (1 + 1 / (c + (r / N) * (r / N))) : 0.0; };
    const double nd = cluster(Nd, 0.21, 4e20), na = cluster(Na, 0.5, 7.2e20);
    const double m = electrons ? me : mh, ratio = electrons ? me / mh : mh / me;
    auto G = [tn, m](double P) {
        return 1 - 0.89233 / std::pow(0.41372 + P * std::pow(tn / m, 0.28227), 0.19778) + 0.005978 / std::pow(P * std::pow(m / tn, 0.72169), 1.80618);
    };
    // (G's minimum: a fine scan.)
    double pmin = 1e-3, gmin = G(pmin);
    for (double P = 1e-3; P < 10; P *= 1.0001)
        if (G(P) < gmin) {
            gmin = G(P);
            pmin = P;
        }
    const double attract = electrons ? nd : na, repel = electrons ? na : nd, other = electrons ? p : n;
    const double Nsc = attract + repel + other;
    const double P = tn * tn / (2.459 / (3.97e13 * std::pow(Nsc, -2.0 / 3.0)) + 3.828 / (1.36e20 / (n + p) * m));
    const double q = std::pow(P, 0.6478);
    // (r3 6.5505: Klaassen's Table 2 - the code's 6.5502, 2e-5 on a minority
    // mobility, was the code's own here: the update check of 21 October.)
    const double F = (0.7643 * q + 2.2999 + 6.5505 * ratio) / (q + 2.3670 - 0.8552 * ratio);
    const double Neff = attract + (P < pmin ? gmin : G(P)) * repel + other / F;
    const double muL = mumax * std::pow(tn, -theta);
    const double muN = mumax * mumax / (mumax - mumin) * std::pow(tn, 3 * alpha - 1.5), muC = mumax * mumin / (mumax - mumin) / std::sqrt(tn);
    const double muDA = muN * (Nsc / Neff) * std::pow(nref / Nsc, alpha) + muC * (n + p) / Neff;
    return 1 / (1 / muL + 1 / muDA);
}

} // namespace

class TestFemMobility : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // Arora's: a uniform bar's electrons' and holes' mobility (semi.mun,
    // semi.mup) its formula's at 1e15, 1e17 and 1e19 cm⁻³ - at 300 K and at
    // a lattice temperature of 400 K, each of its four terms of TL - to
    // 1e-6; at 300 K within 10 % of Masetti's, the other fit of silicon's
    // measured mobilities.
    void aroraByDopingAndTemperature()
    {
        for (const char* N : {"1e15", "1e17", "1e19"})
            for (const char* TL : {"", "400[K]"}) {
                const QString physics = *TL ? QStringLiteral(R"(, "TL": "%1")").arg(QLatin1String(TL)) : QString();
                const Run r = run(modelOf(bar(QLatin1String(N), QStringLiteral("0"), QStringLiteral("1[mV]"), QStringLiteral(R"("mobility": "arora",)"), physics)));
                QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
                const Solution& s = *r.study.solutions.back();
                const double n = std::atof(N), T = *TL ? 400 : 300;
                const double mun = at(s, QStringLiteral("semi.mun"), 1, 0.1) * 1e4, mup = at(s, QStringLiteral("semi.mup"), 1, 0.1) * 1e4;
                qInfo("Arora at %s cm^-3, %g K: electrons %.6g (%.6g), holes %.6g (%.6g) cm^2/(V s); Masetti's %.6g, %.6g", N, T, mun, arora(true, n, T), mup,
                      arora(false, n, T), masetti(true, n), masetti(false, n));
                QVERIFY2(std::abs(mun / arora(true, n, T) - 1) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(mun).arg(arora(true, n, T))));
                QVERIFY2(std::abs(mup / arora(false, n, T) - 1) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(mup).arg(arora(false, n, T))));
                if (!*TL) {
                    QVERIFY2(std::abs(mun / masetti(true, n) - 1) < 0.1, qPrintable(QStringLiteral("%1 %2").arg(mun).arg(masetti(true, n))));
                    QVERIFY2(std::abs(mup / masetti(false, n) - 1) < 0.1, qPrintable(QStringLiteral("%1 %2").arg(mup).arg(masetti(false, n))));
                }
            }
    }

    // Dorkel and Leturcq's: a bar of 1e15 cm⁻³ donors, dark - its formula's
    // at the bar's own carriers - and lit to some 1e17 pairs per cm³: the
    // carriers scattering each other, its mobility its formula's at the
    // point's n and p (each triangle's from its corners' mean) to 1 %, and
    // less than dark.
    void dorkelCarriersScatterEachOther()
    {
        const QString model = QStringLiteral(R"("mobility": "dorkel",)");
        const Run dark = run(modelOf(bar(QStringLiteral("1e15"), QStringLiteral("0"), QStringLiteral("1[mV]"), model)));
        const QString light = QStringLiteral(R"(, {"type": "srh", "source": "user", "taun": "1[us]", "taup": "1[us]", "selection": {"all": true}},
                                               {"type": "generation", "G": "1e33[1/(m^3*s)]", "selection": {"all": true}})");
        const Run lit = run(modelOf(bar(QStringLiteral("1e15"), QStringLiteral("0"), QStringLiteral("1[mV]"), model, QString(), light)));
        for (const Run* r : {&dark, &lit}) QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
        double muDark[2] = {0, 0};
        for (const Run* r : {&dark, &lit}) {
            const Solution& s = *r->study.solutions.back();
            const double n = at(s, QStringLiteral("semi.n"), 1, 0.1) * 1e-6, p = at(s, QStringLiteral("semi.p"), 1, 0.1) * 1e-6;
            const double mun = at(s, QStringLiteral("semi.mun"), 1, 0.1) * 1e4, mup = at(s, QStringLiteral("semi.mup"), 1, 0.1) * 1e4;
            qInfo("Dorkel and Leturcq at n %.4g, p %.4g cm^-3: electrons %.6g (%.6g), holes %.6g (%.6g) cm^2/(V s)", n, p, mun, dorkel(true, 1e15, n, p, 300),
                  mup, dorkel(false, 1e15, n, p, 300));
            QVERIFY2(std::abs(mun / dorkel(true, 1e15, n, p, 300) - 1) < 0.01, qPrintable(QStringLiteral("%1 %2").arg(mun).arg(dorkel(true, 1e15, n, p, 300))));
            QVERIFY2(std::abs(mup / dorkel(false, 1e15, n, p, 300) - 1) < 0.01, qPrintable(QStringLiteral("%1 %2").arg(mup).arg(dorkel(false, 1e15, n, p, 300))));
            if (r == &dark) {
                muDark[0] = mun;
                muDark[1] = mup;
            } else {
                QVERIFY2(n > 1e16 && p > 1e16, qPrintable(QStringLiteral("%1 %2").arg(n).arg(p)));
                QVERIFY2(mun < 0.9 * muDark[0] && mup < 0.9 * muDark[1], qPrintable(QStringLiteral("%1 %2 %3 %4").arg(mun).arg(muDark[0]).arg(mup).arg(muDark[1])));
            }
        }
    }

    // Klaassen's: its formula's (the test's own) at each bar's state - n-type
    // 1e15, 1e17, 1e19, 1e20 cm⁻³ (its minority holes' screening P below
    // G's minimum: G held there) and p-type 1e19, dark, and lit -, to 1e-9
    // (lit 1e-2); the majority electrons to 1e19 within 5 % of Masetti's
    // arsenic fit, whose its parameters are; the minority electrons in p⁺
    // 1e19 more mobile than the majority in n⁺ 1e19 (Swirhun's and others'
    // measurements: its reason).
    void klaassenMajorityAndMinority()
    {
        const QString model = QStringLiteral(R"("mobility": "klaassen",)");
        double majority19 = 0;
        struct Case {
            const char* Nd;
            const char* Na;
            bool lit;
        };
        for (const Case& c : {Case{"1e15", "0", false}, Case{"1e17", "0", false}, Case{"1e19", "0", false}, Case{"1e20", "0", false}, Case{"0", "1e19", false},
                              Case{"1e15", "0", true}}) {
            const QString light = c.lit ? QStringLiteral(R"(, {"type": "srh", "source": "user", "taun": "1[us]", "taup": "1[us]", "selection": {"all": true}},
                                                           {"type": "generation", "G": "1e33[1/(m^3*s)]", "selection": {"all": true}})")
                                        : QString();
            const Run r = run(modelOf(bar(QLatin1String(c.Nd), QLatin1String(c.Na), QStringLiteral("1[mV]"), model, QString(), light)));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.study.solutions.back();
            const double Nd = std::atof(c.Nd), Na = std::atof(c.Na);
            const double n = at(s, QStringLiteral("semi.n"), 1, 0.1) * 1e-6, p = at(s, QStringLiteral("semi.p"), 1, 0.1) * 1e-6;
            const double mun = at(s, QStringLiteral("semi.mun"), 1, 0.1) * 1e4, mup = at(s, QStringLiteral("semi.mup"), 1, 0.1) * 1e4;
            const double en = klaassen(true, Nd, Na, n, p, 300), ep = klaassen(false, Nd, Na, n, p, 300);
            qInfo("Klaassen at Nd %s, Na %s cm^-3%s (n %.4g, p %.4g): electrons %.8g (%.8g, %.2g), holes %.8g (%.8g, %.2g) cm^2/(V s)", c.Nd, c.Na,
                  c.lit ? ", lit" : "", n, p, mun, en, mun / en - 1, mup, ep, mup / ep - 1);
            // (Lit: its triangles' carriers their corners' mean. Dark: to
            // 1e-9 - r3's 2e-5 on the minority electrons in p⁺ caught.)
            const double tolerance = c.lit ? 0.01 : 1e-9;
            QVERIFY2(std::abs(mun / en - 1) < tolerance, qPrintable(QStringLiteral("%1 %2").arg(mun).arg(en)));
            QVERIFY2(std::abs(mup / ep - 1) < tolerance, qPrintable(QStringLiteral("%1 %2").arg(mup).arg(ep)));
            if (Na == 0 && !c.lit && Nd <= 1e19)
                QVERIFY2(std::abs(mun / masetti(true, Nd) - 1) < 0.05, qPrintable(QStringLiteral("%1 %2").arg(mun).arg(masetti(true, Nd))));
            if (Nd == 1e19) majority19 = mun;
            if (Na == 1e19) {
                qInfo("minority electrons in p+ 1e19 %.4g, majority in n+ 1e19 %.4g cm^2/(V s)", mun, majority19);
                QVERIFY2(mun > 1.3 * majority19, qPrintable(QStringLiteral("%1 %2").arg(mun).arg(majority19)));
            }
        }
    }

    // Shirahata's at an insulator: a MOS capacitor's electrons and holes,
    // inverted (2 V) and accumulated (-2 V) - each point's mobility within
    // 30 nm of the interface its formula's at its own field across it, 1/µ
    // = 1/µbulk + 1/µE - 1/µE(0), µE = µ0 (TL/300 K)^-θ / ((1 + E⊥/E1)^P1 +
    // (E⊥/E2)^P2), µbulk the same capacitor's without (its lcrit 1 m: no
    // fading: e^(-d/lcrit) 1 - 1e-8 at 10 nm) - at 300 K and at a lattice
    // temperature of 350 K, to 1e-7.
    // Lombardi's likewise at 350 K: its acoustic phonons' C/(TL E⊥^⅓) of
    // the lattice's temperature, not the physics' T.
    void surfaceModelsAcrossTheInterface()
    {
        // (A sanitized build: 2 V at 350 K alone - its twelve solves past
        // 300 s there.)
        const bool slow = slowBuild();
        for (const char* TL : {"", "350[K]"}) {
            if (slow && !*TL) continue;
            const QString physics = *TL ? QStringLiteral(R"(, "TL": "%1")").arg(QLatin1String(TL)) : QString();
            const double T = *TL ? 350 : 300, tn = T / 300;
            for (const char* Vg : {"2[V]", "-2[V]"}) {
                if (slow && Vg[0] == '-') continue;
                const Run bulk = run(modelOf(mos(QStringLiteral("1e17"), QLatin1String(Vg), QStringLiteral(R"("mobility": "constant",)"), physics)));
                const Run shirahata = run(modelOf(mos(QStringLiteral("1e17"), QLatin1String(Vg),
                                                      QStringLiteral(R"("mobility": "constant", "surface": true, "surfmodel": "shirahata", "lcrit": "1[m]",)"),
                                                      physics)));
                const Run lombardi = run(modelOf(mos(QStringLiteral("1e17"), QLatin1String(Vg),
                                                     QStringLiteral(R"("mobility": "constant", "surface": true, "surfmodel": "lombardi", "lcrit": "1[m]",)"),
                                                     physics)));
                for (const Run* r : {&bulk, &shirahata, &lombardi}) QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
                const Solution &b = *bulk.study.solutions.back(), &s = *shirahata.study.solutions.back(), &l = *lombardi.study.solutions.back();
                double worst = 0, worstL = 0, Emax = 0;
                for (int k = 0; k < 30; ++k) {
                    const double x = 0.2, y = -(k + 0.5) * 0.001;
                    const double across = std::abs(at(s, QStringLiteral("semi.Ey"), x, y)), acrossL = std::abs(at(l, QStringLiteral("semi.Ey"), x, y));
                    Emax = std::max(Emax, across);
                    for (int c = 0; c < 2; ++c) {
                        const QString mu = c == 0 ? QStringLiteral("semi.mun") : QStringLiteral("semi.mup");
                        const double mub = at(b, mu, x, y);
                        // Shirahata's (SI).
                        const double mu0 = (c == 0 ? 0.143 : 0.05) * std::pow(tn, c == 0 ? -2.285 : -2.247);
                        const double E1 = c == 0 ? 6.3e5 : 8e5, E2 = c == 0 ? 7.7e7 : 3.9e7, P1 = c == 0 ? 0.28 : 0.3, P2 = c == 0 ? 2.9 : 1.0;
                        const double muE = mu0 / (std::pow(1 + across / E1, P1) + std::pow(across / E2, P2));
                        const double expect = 1 / (1 / mub + 1 / muE - 1 / mu0);
                        worst = std::max(worst, std::abs(at(s, mu, x, y) / expect - 1));
                        // Lombardi's (B, C, λ, δ silicon's; N 1e17 cm⁻³).
                        const double a = std::max(acrossL, 100.0);
                        const double B = c == 0 ? 4.75e5 : 9.925e4, C = c == 0 ? 1.74e5 : 8.842e5, lambda = c == 0 ? 0.125 : 0.0317, delta = c == 0 ? 5.82e14 : 2.0546e14;
                        const double ac = B / a + 1e-4 * C * std::pow(1e17, lambda) / (T * std::cbrt(a / 100)), sr = delta / (a * a);
                        const double expectL = 1 / (1 / mub + 1 / ac + 1 / sr);
                        worstL = std::max(worstL, std::abs(at(l, mu, x, y) / expectL - 1));
                    }
                }
                qInfo("at %s, TL %g K: E across up to %.4g V/m; Shirahata's within %.3g, Lombardi's within %.3g of their formulas", Vg, T, Emax, worst, worstL);
                QVERIFY2(Emax > 1e7, qPrintable(QString::number(Emax)));
                QVERIFY2(worst < 1e-7, qPrintable(QString::number(worst)));
                QVERIFY2(worstL < 1e-7, qPrintable(QString::number(worstL)));
            }
        }
    }

    // Yamaguchi's: its low-field mobility µL (1 + N/(N/S + Nref))^-½ in a
    // bar at 1 mV (1e15, 1e17, 1e19 cm⁻³); across a MOS capacitor's
    // interface (2 V, its lcrit 1 m), µ0 (1 + as E⊥)^-½ at each point's
    // field; along a 1e17 cm⁻³ bar's current at 1 V (5 kV/cm), µs (1 +
    // x²/(x + G) + (µs E∥/vs)²)^-½, x = µs E∥/uL, at its middle's field - to
    // 1e-6, 1e-7 and 1e-4.
    void yamaguchiAcrossAndAlong()
    {
        auto low = [](bool electrons, double N) {
            const double mul = electrons ? 1400 : 480, S = electrons ? 350 : 81, nref = electrons ? 3e16 : 4e16;
            return mul / std::sqrt(1 + N / (N / S + nref));
        };
        const QString model = QStringLiteral(R"("mobility": "yamaguchi", "ylcrit": "1[m]",)");
        for (const char* N : {"1e15", "1e17", "1e19"}) {
            const Run r = run(modelOf(bar(QLatin1String(N), QStringLiteral("0"), QStringLiteral("1[mV]"), model)));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.study.solutions.back();
            const double n = std::atof(N), mun = at(s, QStringLiteral("semi.mun"), 1, 0.1) * 1e4, mup = at(s, QStringLiteral("semi.mup"), 1, 0.1) * 1e4;
            qInfo("Yamaguchi at %s cm^-3: electrons %.6g (%.6g), holes %.6g (%.6g) cm^2/(V s)", N, mun, low(true, n), mup, low(false, n));
            // (At 1 mV its field along the current a millionth of uL's.)
            QVERIFY2(std::abs(mun / low(true, n) - 1) < 1e-6 && std::abs(mup / low(false, n) - 1) < 1e-6,
                     qPrintable(QStringLiteral("%1 %2 %3 %4").arg(mun).arg(low(true, n)).arg(mup).arg(low(false, n))));
        }
        {
            const Run r = run(modelOf(mos(QStringLiteral("1e17"), QStringLiteral("2[V]"), model)));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.study.solutions.back();
            double worst = 0;
            for (int k = 0; k < 30; ++k) {
                const double y = -(k + 0.5) * 0.001, across = std::abs(at(s, QStringLiteral("semi.Ey"), 0.2, y));
                const double en = low(true, 1e17) * 1e-4 / std::sqrt(1 + 1.54e-7 * across), ep = low(false, 1e17) * 1e-4 / std::sqrt(1 + 5.35e-7 * across);
                worst = std::max({worst, std::abs(at(s, QStringLiteral("semi.mun"), 0.2, y) / en - 1), std::abs(at(s, QStringLiteral("semi.mup"), 0.2, y) / ep - 1)});
            }
            qInfo("Yamaguchi across a MOS capacitor's interface: within %.3g of its formula", worst);
            QVERIFY2(worst < 1e-7, qPrintable(QString::number(worst)));
        }
        {
            const Run r = run(modelOf(bar(QStringLiteral("1e17"), QStringLiteral("0"), QStringLiteral("1[V]"), model)));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.study.solutions.back();
            const double F = std::abs(at(s, QStringLiteral("semi.Ex"), 1, 0.1)), ms = low(true, 1e17) * 1e-4;
            const double u = ms * F / 4.9e4, v = ms * F / 1.036e5, expect = ms / std::sqrt(1 + u * u / (u + 8.8) + v * v);
            const double mun = at(s, QStringLiteral("semi.mun"), 1, 0.1);
            qInfo("Yamaguchi along a bar's current at %.4g V/m: %.6g (%.6g) m^2/(V s), %.4f of its low-field one", F, mun, expect, mun / ms);
            QVERIFY2(std::abs(mun / expect - 1) < 1e-4 && mun / ms < 0.95, qPrintable(QStringLiteral("%1 %2").arg(mun).arg(expect)));
        }
    }

    // The saturation velocities' exponential temperature model: 2.4e7/(1 +
    // 0.8 e^(TL/600 K)) cm/s both (1.035e7 at 300 K, 9.38e6 at 400 K) - a
    // bar of 1e17 cm⁻³ at 10 V saturated by the whole field, its mobility
    // µ0/√(1 + (µ0 E/vsat)²) at its middle's field, µ0 its low-field one
    // (the same bar's at 1 mV), at 300 K and at a lattice temperature of
    // 400 K, to 1e-6.
    void saturationVelocityByTemperature()
    {
        for (const char* TL : {"", "400[K]"}) {
            const QString physics = *TL ? QStringLiteral(R"(, "TL": "%1")").arg(QLatin1String(TL)) : QString();
            const double T = *TL ? 400 : 300, vsat = 2.4e5 / (1 + 0.8 * std::exp(T / 600));
            const Run low = run(modelOf(bar(QStringLiteral("1e17"), QStringLiteral("0"), QStringLiteral("1[mV]"), QStringLiteral(R"("mobility": "constant",)"), physics)));
            const Run high = run(modelOf(bar(QStringLiteral("1e17"), QStringLiteral("0"), QStringLiteral("10[V]"),
                                             QStringLiteral(R"("mobility": "constant", "saturation": true, "driving": "field", "vsatmodel": "exponential",)"), physics)));
            for (const Run* r : {&low, &high}) QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
            const double mu0 = at(*low.study.solutions.back(), QStringLiteral("semi.mun"), 1, 0.1);
            const Solution& s = *high.study.solutions.back();
            const double E = at(s, QStringLiteral("semi.normE"), 1, 0.1), mun = at(s, QStringLiteral("semi.mun"), 1, 0.1);
            const double expect = mu0 / std::sqrt(1 + std::pow(mu0 * E / vsat, 2));
            qInfo("vsat %.6g m/s at %g K: at %.4g V/m %.6g (%.6g) m^2/(V s)", vsat, T, E, mun, expect);
            QVERIFY2(std::abs(mun / expect - 1) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(mun).arg(expect)));
        }
    }

    // A mobility's expression: of the lattice's temperature (1000 cm²/(V s)
    // (300 K/T)² at 400 K: 562.5); of the force along the current, Caughey
    // and Thomas's saturation written out - a bar's current at 10 V the
    // built-in saturation's to 1e-6; of the carriers, 1400/(1 + n/1e17
    // cm⁻³) in a lit bar at its point's n to 1 %; of the field across an
    // interface, 1400/(1 + E⊥/1e7 V/m) in a MOS capacitor at each point's
    // field to 1e-9.
    void anExpressionsMobility()
    {
        {
            const Run r = run(modelOf(bar(QStringLiteral("1e17"), QStringLiteral("0"), QStringLiteral("1[mV]"),
                                          QStringLiteral(R"x("mobility": "expression", "muexpn": "1000[cm^2/(V*s)]*(300[K]/T)^2",)x"), QStringLiteral(R"(, "TL": "400[K]")"))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double mun = at(*r.study.solutions.back(), QStringLiteral("semi.mun"), 1, 0.1) * 1e4;
            qInfo("an expression of T at 400 K: %.8g cm^2/(V s)", mun);
            QVERIFY2(std::abs(mun / 562.5 - 1) < 1e-12, qPrintable(QString::number(mun, 'g', 12)));
        }
        {
            const Run builtIn = run(modelOf(bar(QStringLiteral("1e17"), QStringLiteral("0"), QStringLiteral("10[V]"),
                                                QStringLiteral(R"x("mobility": "expression", "muexpn": "1400[cm^2/(V*s)]", "muexpp": "450[cm^2/(V*s)]", "saturation": true, "Avn": "0", "Avp": "0",)x"))));
            const Run written = run(modelOf(bar(
                QStringLiteral("1e17"), QStringLiteral("0"), QStringLiteral("10[V]"),
                QStringLiteral(R"x("mobility": "expression", "muexpn": "1400[cm^2/(V*s)]/sqrt(1 + (1400[cm^2/(V*s)]*Epar/1e5[m/s])^2)", "muexpp": "450[cm^2/(V*s)]/(1 + 450[cm^2/(V*s)]*Epar/8e4[m/s])",)x"))));
            for (const Run* r : {&builtIn, &written}) QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
            const double a = global(*builtIn.study.solutions.back(), QStringLiteral("semi.I_2")), b = global(*written.study.solutions.back(), QStringLiteral("semi.I_2"));
            qInfo("saturation written as an expression of Epar: %.9g A, built in %.9g A", b, a);
            QVERIFY2(std::abs(b / a - 1) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(b, 0, 'g', 10).arg(a, 0, 'g', 10)));
        }
        {
            const QString light = QStringLiteral(R"(, {"type": "srh", "source": "user", "taun": "1[us]", "taup": "1[us]", "selection": {"all": true}},
                                                   {"type": "generation", "G": "1e33[1/(m^3*s)]", "selection": {"all": true}})");
            const Run r = run(modelOf(bar(QStringLiteral("1e15"), QStringLiteral("0"), QStringLiteral("1[mV]"),
                                          QStringLiteral(R"x("mobility": "expression", "muexpn": "1400[cm^2/(V*s)]/(1 + n/1e17[1/cm^3])",)x"), QString(), light)));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.study.solutions.back();
            const double n = at(s, QStringLiteral("semi.n"), 1, 0.1), mun = at(s, QStringLiteral("semi.mun"), 1, 0.1), expect = 0.14 / (1 + n / 1e23);
            qInfo("an expression of n at %.4g cm^-3: %.6g (%.6g) m^2/(V s)", n * 1e-6, mun, expect);
            QVERIFY2(std::abs(mun / expect - 1) < 0.01 && n > 1e22, qPrintable(QStringLiteral("%1 %2").arg(mun).arg(expect)));
        }
        {
            const Run r = run(modelOf(mos(QStringLiteral("1e17"), QStringLiteral("2[V]"),
                                          QStringLiteral(R"x("mobility": "expression", "muexpn": "1400[cm^2/(V*s)]/(1 + Eperp/1e7[V/m])",)x"))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.study.solutions.back();
            double worst = 0;
            for (int k = 0; k < 30; ++k) {
                const double y = -(k + 0.5) * 0.001, across = std::abs(at(s, QStringLiteral("semi.Ey"), 0.2, y));
                worst = std::max(worst, std::abs(at(s, QStringLiteral("semi.mun"), 0.2, y) / (0.14 / (1 + across / 1e7)) - 1));
            }
            qInfo("an expression of Eperp: within %.3g", worst);
            QVERIFY2(worst < 1e-9, qPrintable(QString::number(worst)));
        }
    }

    // Masetti's constants not a finite number - µ1 or µmax 1/0, m1 or Pc
    // sqrt(-1) - refused, the constant named (the hunt of 21 October, A2:
    // an infinite µ1 solved, a diode's current 170 times too small; the
    // others failed as "its Jacobian is singular", naming nothing).
    void masettisConstantsFinite()
    {
        for (const char* given : {R"("mobility": "masetti", "mu1n": "1/0",)", R"("mobility": "masetti", "mmaxn": "1/0",)",
                                  R"x("mobility": "masetti", "m1p": "sqrt(-1)",)x", R"x("mobility": "masetti", "pcn": "sqrt(-1)",)x"}) {
            const Run r = run(modelOf(bar(QStringLiteral("1e17"), QStringLiteral("0"), QStringLiteral("1[mV]"), QLatin1String(given))));
            const QString key = QString::fromLatin1(given).section(QLatin1Char('"'), 5, 5);
            QVERIFY2(r.error.contains(QStringLiteral("Masetti's mobility, %1").arg(key)) && r.error.contains(QLatin1String("not a finite number"))
                         && !r.error.contains(QLatin1String("singular")) && !r.error.contains(QLatin1String("must be more than 0")),
                     qPrintable(QString::fromLatin1(given) + QLatin1String(": ") + r.error));
        }
    }

    // Masetti's on the library's Silicon: its own µmax, Masetti's fit's 1417
    // and 470.5 cm²/(V s) - the lattice's 1400 and 450 were taken (the
    // update check of 21 October: 718.65 at 1e17 cm⁻³ for 727.05, holes 4 %
    // low at 1e15) -, a bar's mobilities its formula's at 1e15, 1e17 and
    // 1e19 cm⁻³ to 1e-9; at 350 K its µmax (T/300 K)^-ζ, the rest as at
    // 300 K. µmax empty: the lattice's (the Lumerical Masetti model's);
    // given on the feature, that.
    void masettisOwnMaximum()
    {
        const QString model = QStringLiteral(R"("mobility": "masetti",)");
        auto check = [](const Run& r, double N, double T, double mumaxn, double mumaxp) {
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const Solution& s = *r.study.solutions.back();
            const double mun = at(s, QStringLiteral("semi.mun"), 1, 0.1) * 1e4, mup = at(s, QStringLiteral("semi.mup"), 1, 0.1) * 1e4;
            const double en = masetti(true, N, T, mumaxn), ep = masetti(false, N, T, mumaxp);
            qInfo("Masetti at %.3g cm^-3, %g K, µmax %s: electrons %.8g (%.8g), holes %.8g (%.8g) cm^2/(V s)", N, T,
                  mumaxn > 0 ? qPrintable(QStringLiteral("%1, %2").arg(mumaxn).arg(mumaxp)) : "its own", mun, en, mup, ep);
            QVERIFY2(std::abs(mun / en - 1) < 1e-9, qPrintable(QStringLiteral("%1 %2").arg(mun, 0, 'g', 12).arg(en, 0, 'g', 12)));
            QVERIFY2(std::abs(mup / ep - 1) < 1e-9, qPrintable(QStringLiteral("%1 %2").arg(mup, 0, 'g', 12).arg(ep, 0, 'g', 12)));
        };
        for (const char* N : {"1e15", "1e17", "1e19"}) {
            check(run(modelOf(bar(QLatin1String(N), QStringLiteral("0"), QStringLiteral("1[mV]"), model))), std::atof(N), 300, 0, 0);
            if (QTest::currentTestFailed()) return;
        }
        check(run(modelOf(bar(QStringLiteral("1e17"), QStringLiteral("0"), QStringLiteral("1[mV]"), model, QStringLiteral(R"(, "T": "350[K]")")))), 1e17, 350, 0, 0);
        if (QTest::currentTestFailed()) return;
        check(run(modelOf(bar(QStringLiteral("1e17"), QStringLiteral("0"), QStringLiteral("1[mV]"),
                              QStringLiteral(R"("mobility": "masetti", "mmaxn": "", "mmaxp": "",)")))),
              1e17, 300, 1400, 450);
        if (QTest::currentTestFailed()) return;
        check(run(modelOf(bar(QStringLiteral("1e17"), QStringLiteral("0"), QStringLiteral("1[mV]"),
                              QStringLiteral(R"("mobility": "masetti", "mmaxn": "1000[cm^2/(V*s)]", "mmaxp": "400[cm^2/(V*s)]",)")))),
              1e17, 300, 1000, 400);
    }

    // Yamaguchi's is of no temperature, its 300 K fit's: said where the
    // physics' T or a lattice temperature is another (the update check of
    // 21 October: at 400 K the 300 K mobilities to the last digit, not a
    // word) - its mobility the same; at 300 K nothing said.
    void yamaguchiSaysItHasNoTemperature()
    {
        const double expect = 1400 / std::sqrt(1 + 1e17 / (1e17 / 350 + 3e16));
        for (const char* physics : {"", R"(, "T": "400[K]")", R"(, "TL": "350[K]")"}) {
            const Run r = run(modelOf(bar(QStringLiteral("1e17"), QStringLiteral("0"), QStringLiteral("1[mV]"), QStringLiteral(R"("mobility": "yamaguchi",)"),
                                          QLatin1String(physics))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const double mun = at(*r.study.solutions.back(), QStringLiteral("semi.mun"), 1, 0.1) * 1e4;
            const QString said = r.study.warnings.join(QLatin1Char('\n'));
            qInfo("Yamaguchi%s: %.8g (%.8g) cm^2/(V s); %s", physics, mun, expect, qPrintable(said));
            QVERIFY2(std::abs(mun / expect - 1) < 1e-6, qPrintable(QString::number(mun)));
            const QString warning = QStringLiteral("Yamaguchi's mobility is of no temperature - its 300 K fit's");
            if (!*physics) QVERIFY2(!said.contains(warning), qPrintable(said));
            else if (!QLatin1String(physics).contains(QLatin1String("TL"))) QVERIFY2(said.contains(warning + QStringLiteral(" at 400 K")), qPrintable(said));
            else QVERIFY2(said.contains(warning + QStringLiteral(" at a lattice temperature of 350 K")), qPrintable(said));
        }
    }

    // What reads the nearest insulator, named where none meets a
    // semiconductor: Lombardi's surface scattering, Yamaguchi's field
    // across it, an expression of Eperp or dsurf - an expression of n
    // alone nothing (the update check of 21 October: "surface scattering,
    // but no semiconductor meets an insulator" with Yamaguchi's and with an
    // expression of n and p, no surface scattering on).
    void interfaceReadersAreNamed()
    {
        struct Case {
            const char* model;
            const char* said;   // (empty: nothing)
        };
        for (const Case& c : {Case{R"("mobility": "constant", "surface": true, "surfmodel": "lombardi",)", "Lombardi's surface scattering"},
                              Case{R"("mobility": "yamaguchi",)", "Yamaguchi's field across the interface"},
                              Case{R"x("mobility": "expression", "muexpn": "1400[cm^2/(V*s)]/(1 + Eperp/1e7[V/m])",)x", "the mobility's expression of Eperp or dsurf (0 and 1 m)"},
                              Case{R"x("mobility": "expression", "muexpn": "1400[cm^2/(V*s)]/(1 + n/1e17[1/cm^3])",)x", ""}}) {
            const Run r = run(modelOf(bar(QStringLiteral("1e15"), QStringLiteral("0"), QStringLiteral("1[mV]"), QLatin1String(c.model))));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            const QStringList lines = r.study.solutions.back()->log.filter(QStringLiteral("no semiconductor meets an insulator"));
            qInfo("%s: %s", c.model, qPrintable(lines.join(QLatin1Char(' '))));
            if (!*c.said) QVERIFY2(lines.isEmpty(), qPrintable(lines.join(QLatin1Char(' '))));
            else
                QVERIFY2(lines.size() == 1 && lines.first().contains(QLatin1String(c.said) + QStringLiteral(", but no semiconductor meets an insulator: none")),
                         qPrintable(lines.join(QLatin1Char(' '))));
        }
    }

    // A mobility shown in m²/(V·s), not s²·A/kg (the update check of 21
    // October): the variables' unit, a value's, and typed back.
    void mobilityIsShownInItsUnit()
    {
        const Run r = run(modelOf(bar(QStringLiteral("1e15"), QStringLiteral("0"), QStringLiteral("1[mV]"), QStringLiteral(R"("mobility": "constant",)"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const Dim mobility = Dim::of(0, -1, 2, 1);
        int found = 0;
        for (const Variable& v : r.study.solutions.back()->variables())
            if (v.name == QLatin1String("semi.mun") || v.name == QLatin1String("semi.mup")) {
                QCOMPARE(dimName(v.dim), QStringLiteral("m²/(V·s)"));
                ++found;
            }
        QCOMPARE(found, 2);
        QCOMPARE(formatQuantity(0.14, mobility), QStringLiteral("0.14 m²/(V·s)"));
        const std::optional<Unit> typed = parseUnit(QStringLiteral("m²/(V·s)"));
        QVERIFY(typed && typed->dim == mobility && typed->scale == 1);
    }

    // The review's "not checked": parameters as expressions of the
    // physics' T are taken by a solve - Caughey and Thomas's µmin 68.5
    // (T/300 K)^-½ cm²/(V s) at 350 K, its mobility the formula's at it
    // (µ the material's at 350 K, from the same bar's constant one); a
    // saturation velocity 1e7 (300 K/T) cm/s at 350 K (Quay's A 0), a bar's
    // mobility at 10 V µ0/√(1 + (µ0 E/vsat)²) at it - each to 1e-6. (At the
    // uniform T: under a lattice temperature the built-in models follow
    // TL, these do not.)
    void parametersOfTemperature()
    {
        const QString physics = QStringLiteral(R"(, "T": "350[K]")");
        const Run constant = run(modelOf(bar(QStringLiteral("1e17"), QStringLiteral("0"), QStringLiteral("1[mV]"), QStringLiteral(R"("mobility": "constant",)"), physics)));
        const Run doping = run(modelOf(bar(QStringLiteral("1e17"), QStringLiteral("0"), QStringLiteral("1[mV]"),
                                           QStringLiteral(R"x("mobility": "doping", "munmin": "68.5[cm^2/(V*s)]*(T/300[K])^-0.5", "nrefn": "9.2e16[1/cm^3]", "alphan": "0.711",)x"), physics)));
        const Run high = run(modelOf(bar(QStringLiteral("1e17"), QStringLiteral("0"), QStringLiteral("10[V]"),
                                         QStringLiteral(R"x("mobility": "constant", "saturation": true, "driving": "field", "vsatn": "1e7[cm/s]*(300[K]/T)", "Avn": "0",)x"),
                                         physics)));
        for (const Run* r : {&constant, &doping, &high}) QVERIFY2(r->error.isEmpty(), qPrintable(r->error));
        const double lattice = at(*constant.study.solutions.back(), QStringLiteral("semi.mun"), 1, 0.1);
        const double mumin = 68.5e-4 * std::pow(350.0 / 300, -0.5);
        const double expect = mumin + (lattice - mumin) / (1 + std::pow(1e23 / 9.2e22, 0.711));
        const double mun = at(*doping.study.solutions.back(), QStringLiteral("semi.mun"), 1, 0.1);
        const Solution& s = *high.study.solutions.back();
        const double E = at(s, QStringLiteral("semi.normE"), 1, 0.1), vsat = 1e5 * 300 / 350;
        const double saturated = at(s, QStringLiteral("semi.mun"), 1, 0.1), expectSat = lattice / std::sqrt(1 + std::pow(lattice * E / vsat, 2));
        qInfo("at 350 K: µmin of T, %.6g (%.6g); vsat of T, %.6g (%.6g) m^2/(V s)", mun, expect, saturated, expectSat);
        QVERIFY2(std::abs(mun / expect - 1) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(mun).arg(expect)));
        QVERIFY2(std::abs(saturated / expectSat - 1) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(saturated).arg(expectSat)));
    }

    // Hall transport: a bar (16 µm by 2, 1e16 cm⁻³, constant mobility) at
    // 0.1 V across its length, in Bz 0.5 T out of the plane - the Lorentz
    // force's turned currents J = (J0 ∓ µH Bz ẑ×J0)/(1 + (µH Bz)²), µH = r µ.
    // Its field across at its middle r J Bz/(q n), J = |I|/(W t) - its
    // inner micrometre's voltage (the field falls within a Debye length of
    // the insulating sides: the whole width's 2.8 to 3.8 % short at meshes
    // of 0.25 to 0.05 µm, 2 L_D/W 4.1 %), n its middle's, t the thickness
    // (1 m) - to 1e-3 (1e-5 to 1e-4 at meshes of 0.5 to 0.05 µm); electrons'
    // (r 1.15) top below bottom,
    // holes' (r 0.7) above, either way round with Bz reversed; its
    // current the field's none's to 1e-3 (a long bar: the Hall field
    // cancels the force - no magnetoresistance of one carrier); the
    // current density across it (semi.Jy, turned) a thousandth of along;
    // in time the same. A small signal and noise in a field: not solved,
    // and said.
    void hallBar()
    {
        auto hallBar = [](const char* dopant, const char* B, const char* more = "", const char* steps = R"({"type": "stationary"})") {
            return modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "um",
                "geometry": [{"type": "rectangle", "label": "bar", "size": ["16", "2"]}],
                "materials": [{"material": "Silicon", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "Bz": "%2" %3, "features": [
                  {"type": "model", "mobility": "constant", "rhalln": "1.15", "rhallp": "0.7", "selection": {"all": true}},
                  {"type": "doping", "dopant": "%1", "N0": "1e16[1/cm^3]", "selection": {"all": true}},
                  {"type": "ohmic", "name": "1", "V0": "0[V]", "selection": {"of": ["bar"], "side": "left"}},
                  {"type": "ohmic", "name": "2", "V0": "0.1[V]", "selection": {"of": ["bar"], "side": "right"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.25"}}],
              "studies": [{"type": "study", "steps": [%4]}]})json")
                               .arg(QLatin1String(dopant), QLatin1String(B), QLatin1String(more), QLatin1String(steps))
                               .toUtf8());
        };
        for (const char* dopant : {"donor", "acceptor"}) {
            const bool electrons = dopant[0] == 'd';
            const Run none = run(hallBar(dopant, "0[T]"));
            QVERIFY2(none.error.isEmpty(), qPrintable(none.error));
            const double I0 = global(*none.study.solutions.back(), QStringLiteral("semi.I_2"));
            for (const char* B : {"0.5[T]", "-0.5[T]"}) {
                const Run r = run(hallBar(dopant, B));
                QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
                const Solution& s = *r.study.solutions.back();
                const double Bz = B[0] == '-' ? -0.5 : 0.5, rH = electrons ? 1.15 : 0.7;
                const double I = global(s, QStringLiteral("semi.I_2"));
                const double carriers = at(s, electrons ? QStringLiteral("semi.n") : QStringLiteral("semi.p"), 8, 1);
                // (Across its inner micrometre: at the insulating sides the
                // field across falls to none within a Debye length, 41 nm -
                // the whole 2 µm 4 % short of E W.)
                const double dV = at(s, QStringLiteral("semi.V"), 8, 1.5) - at(s, QStringLiteral("semi.V"), 8, 0.5);
                const double whole = at(s, QStringLiteral("semi.V"), 8, 2) - at(s, QStringLiteral("semi.V"), 8, 0);
                const double expect = rH * std::abs(I) * std::abs(Bz) / (Q * carriers * 1.0) * 0.5;
                const double Jx = at(s, QStringLiteral("semi.Jx"), 8, 1), Jy = at(s, QStringLiteral("semi.Jy"), 8, 1);
                qInfo("%s, Bz %s: across its inner micrometre %.6g V (r |I| B/(q n t) of it %.6g V; the whole 2 µm %.6g V), current %.9g A (none's %.9g A), "
                      "Jy/Jx %.3g",
                      electrons ? "electrons" : "holes", B, dV, expect, whole, I, I0, Jy / Jx);
                QVERIFY2(std::abs(std::abs(dV) / expect - 1) < 1e-3, qPrintable(QStringLiteral("%1 %2").arg(dV).arg(expect)));
                // (The current along -x; Bz > 0: electrons pushed up, the top
                // below the bottom; holes up, the top above.)
                QVERIFY2((dV < 0) == ((Bz > 0) == electrons), qPrintable(QString::number(dV)));
                QVERIFY2(std::abs(I / I0 - 1) < 1e-3, qPrintable(QStringLiteral("%1 %2").arg(I).arg(I0)));
                QVERIFY2(std::abs(Jy / Jx) < 1e-3, qPrintable(QStringLiteral("%1 %2").arg(Jy).arg(Jx)));
            }
        }
        // In time: stepped on from rest 1 ns, its field across its rest's to
        // 1e-6 (the turned currents in the step's equations as at rest).
        {
            const Run timed = run(hallBar("donor", "0.5[T]", "", R"x({"type": "stationary"}, {"type": "transient", "times": "range(0, 0.1[ns], 1[ns])", "rtol": "1e-6"})x"));
            QVERIFY2(timed.error.isEmpty(), qPrintable(timed.error));
            Solution& rest = *timed.study.solutions.front();
            Solution& time = *timed.study.solutions.back();
            time.select(time.snapshotCount() - 1);
            auto inner = [](const Solution& s) { return at(s, QStringLiteral("semi.V"), 8, 1.5) - at(s, QStringLiteral("semi.V"), 8, 0.5); };
            qInfo("in time, 1 ns on: across its inner micrometre %.9g V, at rest %.9g V", inner(time), inner(rest));
            QVERIFY2(std::abs(inner(time) / inner(rest) - 1) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(inner(time)).arg(inner(rest))));
        }
        // (Not solved, and said: a small signal in a field, noise in one.)
        const Run frequency = run(hallBar("donor", "0.5[T]", "", R"({"type": "stationary"}, {"type": "frequency", "frequencies": "1[MHz]"})"));
        qInfo("a small signal in a field: %s", qPrintable(frequency.error));
        QVERIFY2(frequency.error.contains(QLatin1String("magnetic field")), qPrintable(frequency.error));
        const Run noise = run(hallBar("donor", "0.5[T]", R"(, "noise": true)"));
        qInfo("noise in a field: %s", qPrintable(noise.error));
        QVERIFY2(noise.error.contains(QLatin1String("magnetic field")), qPrintable(noise.error));
    }
};

QTEST_GUILESS_MAIN(TestFemMobility)
#include "test_fem_mobility.moc"
