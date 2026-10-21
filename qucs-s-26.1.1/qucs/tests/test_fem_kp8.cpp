/*
 * test_fem_kp8.cpp - the 8x8 k·p (Kane's) of the quantum wells against what
 *                    is known: Kane's bulk band, the 6x6's heavy holes, De
 *                    Franceschi et al.'s tight binding (Lumerical's 8x8
 *                    benchmark), the gain's crossing, its electrons' density,
 *                    the absorption's edge against the 6x6's
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_edge.h"
#include "fem_materials.h"
#include "fem_quantum_testing.h"
#include "fem_results.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

using namespace qucs_s::fem;
using namespace femtest;

class TestFemKp8 : public QObject
{
    Q_OBJECT

private slots:
    void kp8AgainstKane()
    {
        // The 8x8 (Kane's) in kaneBox: in the bulk its electrons are the root
        // of Kane's (E - Ec - x)(E - Ev')(E - Ev' + Δ) = Ep x (E - Ev' + 2Δ/3),
        // x = ħ²k²/2m0, Ev' = Ev - x - non-parabolic. The box's lowest
        // electron along k in the plane that root at k² + kz² (kz the one of
        // its energy at k = 0): to 0.3 meV where a parabola is 15 meV off.
        // Its heavy holes at k = 0 the 6x6's (no S in them there), to 1e-6
        // eV.
        const double Eg = KaneEg, D = KaneD, Ep = KaneEp, me = KaneMe;
        const Run r = run(modelOf(kaneBox("kp8", QStringLiteral(R"({"type": "bands", "kmax": "0.6[1/nm]", "points": 4, "states": 2})"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        auto kane = [&](double k2) {   // (k² in 1/nm²; the root above the gap, Newton's from the parabola)
            const double x = HB2M * k2, ev = -Eg - x;
            auto f = [&](double E) { return (E - x) * (E - ev) * (E - ev + D) - Ep * x * (E - ev + 2 * D / 3); };
            double E = x / me;
            for (int it = 0; it < 100; ++it) {
                const double h = 1e-7, d = (f(E + h) - f(E - h)) / (2 * h);
                const double step = f(E) / d;
                E -= step;
                if (std::abs(step) < 1e-14) break;
            }
            return E;
        };
        s.select(0);
        const double e0 = eV(s, QStringLiteral("qw.Ec1"));
        QVERIFY2(std::abs(e0 - eV(s, QStringLiteral("qw.Ec2"))) < 1e-9, "a Kramers pair");
        // kz of the box: Kane's root at kz² its energy (bisection).
        double lo = 0, hi = 1;
        for (int it = 0; it < 100; ++it) (kane((lo + hi) / 2) < e0 ? lo : hi) = (lo + hi) / 2;
        const double kz2 = (lo + hi) / 2;
        qInfo("8x8: e1 at k = 0 %.6f eV (kz %.4f /nm; pi/L %.4f)", e0, std::sqrt(kz2), M_PI / 50);
        double worst = 0, parabola = 0;
        for (int point = 1; point < s.snapshotCount(); ++point) {
            s.select(point);
            const double k = global(s, QStringLiteral("qw.k")) * 1e-9, got = eV(s, QStringLiteral("qw.Ec1")), want = kane(k * k + kz2);
            qInfo("k %.2f /nm: e1 %.6f eV, Kane's %.6f, a parabola's %.6f", k, got, want, e0 + HB2M * k * k / me);
            worst = std::max(worst, std::abs(got - want));
            parabola = std::max(parabola, std::abs(e0 + HB2M * k * k / me - want));
        }
        QVERIFY2(worst < 3e-4 && parabola > 0.01, qPrintable(QStringLiteral("%1 (a parabola %2)").arg(worst).arg(parabola)));
        // The heavy holes at k = 0: the 6x6's.
        const Run r6 = run(modelOf(kaneBox("kp6", QStringLiteral(R"({"type": "bands", "kmax": "0.6[1/nm]", "points": 2, "states": 2})"))));
        QVERIFY2(r6.error.isEmpty(), qPrintable(r6.error));
        s.select(0);
        r6.solution->select(0);
        const double hh8 = eV(s, QStringLiteral("qw.Ev1")), hh6 = eV(*r6.solution, QStringLiteral("qw.Ev1"));
        QVERIFY2(std::abs(hh8 - hh6) < 1e-6, qPrintable(QStringLiteral("%1 against %2").arg(hh8).arg(hh6)));
    }

    void kp8ElectronsMass()
    {
        // A spectrum's electrons in kaneBox: their energy at k = 0 the bands
        // step's, their mass in the plane me's (the box's non-parabolicity a
        // few parts in a thousand: to 1 %). (Under the sanitizers left out:
        // its 8x8 spectrum, Eigen's complex solves unoptimized, past QtTest's
        // 300 s; the Release jobs run it.)
        if (slowBuild()) QSKIP("the 8x8 spectrum past 300 s under the sanitizers: the Release jobs run it");
        const Run b = run(modelOf(kaneBox("kp8", QStringLiteral(R"({"type": "bands", "kmax": "0.6[1/nm]", "points": 2, "states": 1})"))));
        QVERIFY2(b.error.isEmpty(), qPrintable(b.error));
        b.solution->select(0);
        const double e0 = eV(*b.solution, QStringLiteral("qw.Ec1"));
        const Run sp = run(modelOf(kaneBox("kp8", QStringLiteral(R"x({"type": "spectrum", "energies": "linspace(1.50[eV], 1.60[eV], 3)", "states": 1, "linewidth": "5[meV]", "kpoints": %1})x")
                                                          .arg(41))));
        QVERIFY2(sp.error.isEmpty(), qPrintable(sp.error));
        const QString log = sp.solution->log.join(QLatin1Char('\n'));
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("the 8x8's electrons at k = 0 ([-0-9.e]+) eV \\(the effective mass's ([-0-9.e]+)\\), their masses in the plane near it ([0-9.e]+) m0")).match(log);
        QVERIFY2(m.hasMatch(), qPrintable(log));
        qInfo("the spectrum's e1 %s eV (the effective mass's %s), its mass %s m0 (me %.5f)", qPrintable(m.captured(1)), qPrintable(m.captured(2)), qPrintable(m.captured(3)), KaneMe);
        QVERIFY2(std::abs(m.captured(3).toDouble() / KaneMe - 1) < 0.01, qPrintable(m.captured(3)));
        QVERIFY2(std::abs(m.captured(1).toDouble() - e0) < 1e-4, qPrintable(m.captured(1)));
    }

    void kp8AgainstTightBinding()
    {
        // Lumerical's 8x8 benchmark: De Franceschi, Jancu and Beltram's
        // GaAs/AlAs wells (PRB 59, 9691, 1999; 4.8 nm of GaAs between 12 nm
        // of AlAs - three alike, barely coupled: one here), their tight-
        // binding valence subbands near k = 0 (from each subband's top at the
        // first k; the first's at it 0). The 8x8 within 0.5 meV of the first
        // two, 1.5 of the third (the 6x6's 0.3, 0.3, 1.3; the S-Z coupling
        // symmetrized, (P kz + kz P)/2, put the second 3 meV off).
        const QString json = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "nm",
            "geometry": [{"type": "rectangle", "name": "below", "size": ["10", "12"]},
                         {"type": "rectangle", "name": "well", "position": ["0", "12"], "size": ["10", "4.8"]},
                         {"type": "rectangle", "name": "above", "position": ["0", "16.8"], "size": ["10", "12"]}],
            "physics": [{"type": "quantum", "tag": "qw", "valence": "kp8", "h1d": "%1[nm]", "features": [
              {"type": "material", "source": "user", "Ec": "1.42248[eV]", "Ev": "0[eV]", "me": "0.067", "mhh": "0.327869", "mlh": "0.090498",
               "gamma3": "3", "Delta": "0.341[eV]", "Ep": "25.7[eV]", "nr": "3.25", "selection": {"only": ["well"]}},
              {"type": "material", "source": "user", "Ec": "2.52337[eV]", "Ev": "-0.47967[eV]", "me": "0.15", "mhh": "0.4", "mlh": "0.190114",
               "gamma3": "1.46", "Delta": "0.28[eV]", "Ep": "21.1[eV]", "nr": "3.25", "selection": {"only": ["below", "above"]}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
          "studies": [{"type": "study", "steps": [{"type": "bands", "kmax": "0.12[1/nm]", "points": 13, "states": 3}]}]})json")
                                 .arg(slowBuild() ? QStringLiteral("0.2") : QStringLiteral("0.1"));
        const Run r = run(modelOf(json.toUtf8()));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        // (k in 1/Å, E in eV: the reference's points.)
        const double ref[3][3][2] = {{{0.0012986, -0.0341002}, {0.0069873, -0.0347063}, {0.0106073, -0.0358288}},
                                     {{0.0012986, -0.0715137}, {0.0069873, -0.0715137}, {0.0108659, -0.0715137}},
                                     {{0.0023329, -0.1332073}, {0.0080216, -0.1343523}, {0.0113830, -0.1359687}}};
        std::vector<double> ks;
        std::vector<std::array<double, 3>> tops;   // each subband's top, Ev1, Ev3, Ev5
        for (int point = 0; point < s.snapshotCount(); ++point) {
            s.select(point);
            ks.push_back(global(s, QStringLiteral("qw.k")) * 1e-10);   // 1/Å
            tops.push_back({eV(s, QStringLiteral("qw.Ev1")), eV(s, QStringLiteral("qw.Ev3")), eV(s, QStringLiteral("qw.Ev5"))});
        }
        auto at = [&](int band, double k) {
            for (std::size_t i = 1; i < ks.size(); ++i)
                if (ks[i] >= k) return tops[i - 1][std::size_t(band)] + (tops[i][std::size_t(band)] - tops[i - 1][std::size_t(band)]) * (k - ks[i - 1]) / (ks[i] - ks[i - 1]);
            return tops.back()[std::size_t(band)];
        };
        const double zero = at(0, ref[0][0][0]);
        double worst[3] = {0, 0, 0};
        for (int band = 0; band < 3; ++band) {
            QStringList said;
            for (int p = 0; p < 3; ++p) {
                const double got = at(band, ref[band][p][0]) - zero, want = ref[band][p][1] - ref[0][0][1];
                worst[band] = std::max(worst[band], std::abs(got - want));
                said << QStringLiteral("%1 (%2)").arg(got * 1e3, 0, 'f', 2).arg(want * 1e3, 0, 'f', 2);
            }
            qInfo("subband %d: %s meV", band + 1, qPrintable(said.join(QStringLiteral(", "))));
        }
        QVERIFY2(worst[0] < 5e-4 && worst[1] < 5e-4 && worst[2] < 1.5e-3,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(worst[0]).arg(worst[1]).arg(worst[2])));
        // Its Ep lowered in both (a remote term below a free electron's),
        // said; its conduction states the 8x8's too.
        s.select(0);
        QVERIFY(eV(s, QStringLiteral("qw.Ec1")) > 1.42248 && eV(s, QStringLiteral("qw.Ec1")) < 1.42248 + 0.4);
    }

    void kp8Gain()
    {
        // The 8x8's gain (3e18 cm⁻³ in the infinite well, unbroadened): its
        // electrons' density of its own bands, their occupations with the
        // holes' - crossing 0 at the quasi-Fermi levels' separation
        // (Bernard and Duraffourg); the electrons' level below parabolas'
        // (kp8EdgeStep its absorption's edge).
        auto spectrum = [&](const char* valence, const QString& more, const char* linewidth = "0[eV]") {
            QByteArray json = box(QStringLiteral("20"), QStringLiteral("10"), QStringLiteral(R"(, "valence": "%1")").arg(QLatin1String(valence)), QStringLiteral("0[eV]"),
                                  QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.55[eV], 1.80[eV], 251)", "states": %1, "linewidth": "%3"%2})x")
                                      .arg(slowBuild() ? 2 : 3)
                                      .arg(more, QLatin1String(linewidth)));
            json.replace(R"("nr": "3.6")", R"("nr": "3.6", "gamma3": "2.93", "Delta": "0.34[eV]")");
            return run(modelOf(lighter(json)));
        };
        const Run r = spectrum("kp8", QStringLiteral(R"(, "density": "3e18[1/cm^3]")"));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        s.select(0);
        const double dF = global(s, QStringLiteral("qw.dF"));
        double crossing = NAN, peak = 0;
        for (int k = 1; k < s.snapshotCount(); ++k) {
            s.select(k - 1);
            const double g0 = global(s, QStringLiteral("qw.gTE")), e0 = global(s, QStringLiteral("qw.Ephoton"));
            s.select(k);
            const double g1 = global(s, QStringLiteral("qw.gTE")), e1 = global(s, QStringLiteral("qw.Ephoton"));
            peak = std::max(peak, g1);
            if (g0 > 0 && g1 <= 0 && std::isnan(crossing)) crossing = e0 + (e1 - e0) * g0 / (g0 - g1);
        }
        qInfo("8x8 gain: peak %.0f /cm; it crosses 0 at %.4f eV, the quasi-Fermi levels %.4f eV apart", peak / 100, crossing / Q, dF / Q);
        QVERIFY(peak > 0);
        QVERIFY2(std::abs(crossing - dF) < 3e-4 * Q, qPrintable(QString::number((crossing - dF) / Q)));
        // The electrons counted in their own bands: heavier as they rise, more
        // per energy than parabolas of their masses at the bottom hold - the
        // quasi-Fermi level below those parabolas' (a few meV at 3e18).
        {
            const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("the 8x8's electrons at k = 0 ([-0-9., e]+) eV \\(the effective mass's [^)]*\\), their masses in the plane near it ([0-9., e]+) m0"))
                                                  .match(s.log.join(QLatin1Char('\n')));
            QVERIFY2(m.hasMatch(), qPrintable(s.log.join(QLatin1Char('\n'))));
            const QStringList es = m.captured(1).split(QStringLiteral(", ")), ms = m.captured(2).split(QStringLiteral(", "));
            const double kT = KB * 300, want = 3e24 * 10e-9;
            auto sheet = [&](double F) {
                double n = 0;
                for (int i = 0; i < es.size(); ++i) {
                    const double x = (F - es[i].toDouble() * Q) / kT;
                    n += ms[i].toDouble() * M0 * kT / (M_PI * Hbar * Hbar) * (x > 30 ? x : std::log1p(std::exp(x)));
                }
                return n;
            };
            double lo = -1 * Q, hi = 2 * Q;
            for (int it = 0; it < 200; ++it) (sheet((lo + hi) / 2) < want ? lo : hi) = (lo + hi) / 2;
            s.select(0);
            const double Fc = global(s, QStringLiteral("qw.Fc"));
            qInfo("8x8 electrons: Fc %.5f eV; parabolas of their masses %.5f", Fc / Q, lo / Q);
            QVERIFY2(Fc < lo - 0.002 * Q && Fc > lo - 0.03 * Q, qPrintable(QString::number((Fc - lo) / Q)));
        }
    }

    void kp8EdgeStep()
    {
        // kp8Gain's well: the 8x8's TE absorption at the e1-hh1 edge the 6x6's
        // as their reduced masses - the 8x8's electron heavier in the plane,
        // its band not a parabola: 8.6 % more here -, to 3 % (the electron's
        // S part a little less than the whole). With excitons (they were left
        // out: wishlist 4.4 of the check of 21 October) each pair's own 1s,
        // its electron the 8x8's state, heavier in the plane: its e1-hh1 bound
        // a little more than the 6x6's, no more than their reduced masses'
        // 1.10 (9.33 meV, 8.89); the coupled model asked for,
        // the per-pair one taken and said.
        auto spectrum = [&](const char* valence, const QString& more, const char* linewidth = "0[eV]", int energies = 251) {
            QByteArray json = box(QStringLiteral("20"), QStringLiteral("10"), QStringLiteral(R"(, "valence": "%1")").arg(QLatin1String(valence)), QStringLiteral("0[eV]"),
                                  QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.55[eV], 1.80[eV], %4)", "states": %1, "linewidth": "%3"%2})x")
                                      .arg(slowBuild() ? 2 : 3)
                                      .arg(more, QLatin1String(linewidth))
                                      .arg(energies));
            json.replace(R"("nr": "3.6")", R"("nr": "3.6", "gamma3": "2.93", "Delta": "0.34[eV]")");
            // (Under the sanitizers its k at 21 values, not lighter's 41.)
            QByteArray light = lighter(json);
            if (slowBuild()) light.replace(R"("kpoints": 41)", R"("kpoints": 21)");
            return run(modelOf(light));
        };
        // (Under the sanitizers the 8x8's alone, bound, and none of the rest:
        // its spectra past QtTest's 300 s there; the Release jobs run them.)
        const Run x8 = spectrum("kp8", QStringLiteral(R"(, "excitons": true)"), "5[meV]", slowBuild() ? 1 : 3);
        QVERIFY2(x8.error.isEmpty(), qPrintable(x8.error));
        if (slowBuild()) {
            x8.solution->select(0);
            QVERIFY(global(*x8.solution, QStringLiteral("qw.Eb")) > 0);
            return;
        }
        const Run x6 = spectrum("kp6", QStringLiteral(R"(, "excitons": true)"), "5[meV]", 3);
        QVERIFY2(x6.error.isEmpty(), qPrintable(x6.error));
        x8.solution->select(0);
        x6.solution->select(0);
        const double b8 = global(*x8.solution, QStringLiteral("qw.Eb")), b6 = global(*x6.solution, QStringLiteral("qw.Eb"));
        qInfo("e1-hh1 bound by %.4f meV under the 8x8, %.4f under the 6x6", b8 / Q * 1e3, b6 / Q * 1e3);
        QVERIFY2(b8 > 0 && b8 >= b6 && b8 < 1.1 * b6, qPrintable(QStringLiteral("%1 %2").arg(b8).arg(b6)));
        const Run coupled = spectrum("kp8", QStringLiteral(R"(, "excitons": true, "excitonmodel": "coupled")"), "5[meV]", 3);
        QVERIFY2(coupled.error.isEmpty() && coupled.solution->log.join(QLatin1Char(' ')).contains(QLatin1String("the 8x8 bands' excitons each pair's own 1s")),
                 qPrintable(coupled.error + coupled.solution->log.join(QLatin1Char('\n'))));
        // The edge's step: the absorption just above e1-hh1 (k = 0), against
        // the 6x6's. (Under the sanitizers left out: two spectra past QtTest's
        // 300 s there; the Release jobs run them.)
        if (slowBuild()) return;
        const Run a8 = spectrum("kp8", QString()), a6 = spectrum("kp6", QString());
        QVERIFY2(a8.error.isEmpty() && a6.error.isEmpty(), qPrintable(a8.error + a6.error));
        auto step = [&](Solution& sol) {
            sol.select(0);
            const double edge = global(sol, QStringLiteral("qw.Ee1hh1"));
            for (int k = 0; k < sol.snapshotCount(); ++k) {
                sol.select(k);
                if (global(sol, QStringLiteral("qw.Ephoton")) > edge + 0.005 * Q) return global(sol, QStringLiteral("qw.alphaTE"));
            }
            return double(NAN);
        };
        const double s8 = step(*a8.solution), s6 = step(*a6.solution);
        auto mass = [](const Run& run, const char* pattern) {
            const QRegularExpressionMatch m = QRegularExpression(QLatin1String(pattern)).match(run.solution->log.join(QLatin1Char('\n')));
            return m.hasMatch() ? m.captured(1).toDouble() : double(NAN);
        };
        const double me8 = mass(a8, "the 8x8's electrons at k = 0 .*? their masses in the plane near it ([0-9.e]+)");
        const double mh6 = mass(a6, "valence bands .* their masses in the plane near it ([0-9.e]+)");
        const double mh8 = mass(a8, "valence bands .* their masses in the plane near it ([0-9.e]+)");
        const double mr8 = 1 / (1 / me8 + 1 / mh8), mr6 = 1 / (1 / 0.067 + 1 / mh6);
        qInfo("the e1-hh1 step: the 8x8's %.0f /cm, the 6x6's %.0f (%.4f; their reduced masses' %.4f: me %.4f, mhh %.4f and %.4f)", s8 / 100, s6 / 100,
              s8 / s6, mr8 / mr6, me8, mh8, mh6);
        QVERIFY2(s8 / s6 / (mr8 / mr6) > 0.97 && s8 / s6 / (mr8 / mr6) < 1.005, qPrintable(QString::number(s8 / s6 / (mr8 / mr6))));
    }

    // Miller's 96 Å GaAs well in Al0.3Ga0.7As under the 8x8 (the check of
    // 21 October), Chao and Chuang's Luttinger parameters (GaAs γ1 6.8, γ2
    // 1.9): Ep lowered for A_c 1 alone (21.19 eV) left the light holes'
    // remote term along the axis γ1 + 4γ2 - Ep/Eg at -0.48 - their Z
    // orbital's band turned into the gap at large kz, spurious states at
    // 0.203 and 0.655 eV (its electrons from 0.624, their S share 0.28 and
    // 0.32), every pair left out and "0 transitions", said in the log alone.
    // Now Ep lowered to make that 1 too (19.06 eV), said; none spurious; its
    // electrons above the well's edge, e1 within 2 meV of the effective
    // mass's, e2 below it (non-parabolic); the spectrum's transitions there.
    // The States step under the 8x8 its electrons' energies (it gave the
    // effective mass's, not a word: 2.2), under the 4x4 the effective
    // mass's.
    void aGaAsWellOfChaoAndChuang()
    {
        if (slowBuild()) QSKIP("its 8x8 spectrum past 300 s under the sanitizers: the Release jobs run it");
        auto miller = [](const char* valence, const QString& step) {
            QByteArray json = heterostructure(QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"),
                                              QStringLiteral("9.6"), QStringLiteral("20"),
                                              QStringLiteral(R"(, "valence": "%1", "h1d": "0.1[nm]")").arg(QLatin1String(valence)), step);
            json.replace(R"("compound": "GaAs", "x": "0")", R"("compound": "GaAs", "x": "0", "gamma1": "6.80", "gamma2": "1.90", "gamma3": "2.73")");
            json.replace(R"("compound": "AlGaAs", "x": "0.3")", R"("compound": "AlGaAs", "x": "0.3", "gamma1": "5.795", "gamma2": "1.534", "gamma3": "2.568")");
            return run(modelOf(json));
        };
        const QString spectrum = QStringLiteral(R"x({"type": "spectrum", "energies": "linspace(1.42[eV], 1.80[eV], 20)", "cstates": 4, "vstates": 6,
                                                   "continuum": "bound", "linewidth": "5.5[meV]", "lineshape": "gaussian", "kpoints": 41})x");
        const Run r = miller("kp8", spectrum);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const QString log = r.solution->log.join(QLatin1Char('\n'));
        const QStringList said = r.solution->log.filter(QLatin1String("the 8x8's electrons at k = 0"));
        QVERIFY2(said.size() == 1, qPrintable(log));
        qInfo("%s", qPrintable(said.front()));
        QVERIFY2(said.front().contains(QRegularExpression(QStringLiteral("domain 2's Ep 28.8 eV to 19.0[0-9] eV \\(its light holes', γ1 \\+ 4γ2 14.4\\)"))),
                 qPrintable(said.front()));
        QVERIFY2(!log.contains(QLatin1String("spurious")) && !log.contains(QLatin1String("no transitions")), qPrintable(log));
        // Its electrons (k = 0) and the effective mass's, from the log.
        const QRegularExpression re(QStringLiteral("k = 0 ([0-9., ]+) eV \\(the effective mass's ([0-9., ]+)\\)"));
        const QRegularExpressionMatch m = re.match(said.front());
        QVERIFY2(m.hasMatch(), qPrintable(said.front()));
        const QStringList e8 = m.captured(1).split(QStringLiteral(", ")), em = m.captured(2).split(QStringLiteral(", "));
        QVERIFY(e8.size() >= 2 && em.size() >= 2);
        const double edge = 0.624;   // (the well's conduction edge, eV: GaAs's)
        for (const QString& e : e8) QVERIFY2(e.toDouble() > edge, qPrintable(said.front()));
        QVERIFY2(std::abs(e8[0].toDouble() - em[0].toDouble()) < 0.002 && e8[1].toDouble() < em[1].toDouble() - 0.001, qPrintable(said.front()));
        const QStringList count = r.solution->log.filter(QLatin1String("a spectrum of"));
        QVERIFY2(count.size() == 1 && !count.front().contains(QLatin1String("a spectrum of 0 ")), qPrintable(log));
        // The States step: the 8x8's energies (said), the 4x4's the effective mass's.
        const Run s8 = miller("kp8", QStringLiteral(R"({"type": "states", "states": 4})")), s4 = miller("kp4", QStringLiteral(R"({"type": "states", "states": 4})"));
        QVERIFY2(s8.error.isEmpty() && s4.error.isEmpty(), qPrintable(s8.error + s4.error));
        const double ee8[2] = {eV(*s8.solution, QStringLiteral("qw.Ee1")), eV(*s8.solution, QStringLiteral("qw.Ee2"))};
        const double ee4[2] = {eV(*s4.solution, QStringLiteral("qw.Ee1")), eV(*s4.solution, QStringLiteral("qw.Ee2"))};
        qInfo("States: kp8 e1 %.5f, e2 %.5f eV; kp4 %.5f, %.5f eV", ee8[0], ee8[1], ee4[0], ee4[1]);
        for (int i = 0; i < 2; ++i) {
            QVERIFY2(std::abs(ee8[i] - e8[i].toDouble()) < 2e-5, qPrintable(QStringLiteral("%1 %2").arg(ee8[i]).arg(e8[i])));
            QVERIFY2(std::abs(ee4[i] - em[i].toDouble()) < 2e-5, qPrintable(QStringLiteral("%1 %2").arg(ee4[i]).arg(em[i])));
        }
        QVERIFY2(s8.solution->log.join(QLatin1Char(' ')).contains(QStringLiteral("the 8x8 k·p's energies at k = 0 for 4 electrons")),
                 qPrintable(s8.solution->log.join(QLatin1Char('\n'))));
        QVERIFY(!s4.solution->log.join(QLatin1Char(' ')).contains(QStringLiteral("k·p's energies at k = 0")));
    }

    // A material whose parameters make spurious states Ep cannot help: a
    // γ3 of 10 (γ3' 7.2 against γ1' 1 - not elliptic in the plane, the
    // valence bands turned into the gap at large k): the states not their
    // band's, on the wrong side of its edge, or oscillating, left out and
    // said - no electron's taken all the same (the conduction states' search
    // shifted with them as they rise: at the band's edge the spurious ones
    // filled it at 5 values of k, taken as electrons; the holes' 2, among
    // its spurious band); the electrons above the
    // conduction edge, the holes below the valence's; the spectrum's
    // transitions there.
    void spuriousStatesLeftOut()
    {
        if (slowBuild()) QSKIP("its 8x8 spectrum's spurious band past 300 s under the sanitizers: the Release jobs run it");
        QByteArray json = kaneBox("kp8", QStringLiteral(R"x({"type": "spectrum", "energies": "linspace(1.5[eV], 1.8[eV], 4)", "cstates": 2, "vstates": 4,
                                                           "linewidth": "5[meV]", "lineshape": "gaussian", "kpoints": 21})x"));
        const QByteArray g3 = QStringLiteral(R"("gamma3": "%1")").arg(KaneEp / (6 * KaneEg), 0, 'g', 12).toUtf8();
        QVERIFY(json.contains(g3));
        json.replace(g3, R"("gamma3": "10")");
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const QString log = r.solution->log.join(QLatin1Char('\n'));
        qInfo("%s", qPrintable(r.solution->log.filter(QLatin1String("8x8's electrons")).join(QString())));
        QVERIFY2(log.contains(QLatin1String("spurious states left out")) && !log.contains(QRegularExpression(QStringLiteral("taken all the same, too few else: [1-9]"))),
                 qPrintable(log));
        const QStringList count = r.solution->log.filter(QLatin1String("a spectrum of"));
        QVERIFY2(count.size() == 1 && !count.front().contains(QLatin1String("a spectrum of 0 ")), qPrintable(log));
        r.solution->select(0);
        const double e1 = eV(*r.solution, QStringLiteral("qw.Ee1")), h1 = eV(*r.solution, QStringLiteral("qw.Ehh1"));
        QVERIFY2(e1 > 0 && h1 < -KaneEg + 1e-9, qPrintable(QStringLiteral("%1 %2").arg(e1).arg(h1)));
    }

    // A spectrum of no transitions between the subbands - every electron
    // left out at its walls (a cutoff of 0) - said as a warning, not a
    // line of the log alone (the check of 21 October's "0 transitions").
    void noTransitionsSaid()
    {
        const Run r = run(modelOf(heterostructure(QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"), QStringLiteral("8"),
                                                  QStringLiteral("10"), QStringLiteral(R"(, "h1d": "0.5[nm]")"),
                                                  QStringLiteral(R"x({"type": "spectrum", "energies": "linspace(1.45[eV], 1.6[eV], 4)", "states": 2,
                                                                     "cutoff_c": "0", "linewidth": "5[meV]"})x"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QVERIFY2(r.study.warnings.join(QLatin1Char('\n')).contains(QLatin1String("a spectrum of no transitions between the subbands")),
                 qPrintable(r.solution->log.join(QLatin1Char('\n'))));
    }
};

QTEST_MAIN(TestFemKp8)
#include "test_fem_kp8.moc"
