/*
 * test_fem_nitrides.cpp - wurtzite quantum wells against what is known:
 *                         Chuang and Chang's closed forms and their 1997
 *                         well (Lumerical's 6x6 benchmark), the c-plane's
 *                         isotropy, the interfaces' order, the nitrides'
 *                         library, TE and TM, the polarization's fields and
 *                         their screening; Lumerical's MQW projects read
 *                         (their HDF5) and made models
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
#include "fem_hdf5.h"
#include "fem_lumerical.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

using namespace qucs_s::fem;
using namespace femtest;

class TestFemNitrides : public QObject
{
    Q_OBJECT

private slots:
    void wurtziteAtTheZone()
    {
        // A box of strained GaN (Chuang and Chang's, εxx -0.5 %, εzz -2
        // C13/C33 εxx), 10 nm with hard walls, at k = 0: the heavy holes alone
        // (no k in the plane: nothing couples them), Ev + Δ1 + Δ2 + λε + θε +
        // (A1 + A3) q², the light and crystal-field holes the 2x2 of Δ1 - Δ2 +
        // λε + θε + (A1 + A3) q² and λε + A1 q² coupled by √2 Δ3 - q = nπ/L
        // each (λε = D1 εzz + 2 D2 εxx, θε = D3 εzz + 2 D4 εxx). The highest
        // six to 1e-5 eV; the electron Ec + a1 εzz + 2 a2 εxx + ħ²q²/2me.
        const double exx = -0.005, ezz = -2 * 106.0 / 398 * exx, L = 10;
        const double A1 = -6.56, A3 = 5.65, D1 = -0.89, D2 = 4.27, D3 = 5.18, D4 = -2.59, d1 = 0.021, d2 = 0.016 / 3;
        const double lam = D1 * ezz + 2 * D2 * exx, th = D3 * ezz + 2 * D4 * exx;
        std::vector<double> levels;
        for (int n = 1; n <= 8; ++n) {
            const double q2 = std::pow(n * M_PI / L, 2) * HB2M;
            levels.push_back(0.108256 + d1 + d2 + lam + th + (A1 + A3) * q2);
            const double a = d1 - d2 + lam + th + (A1 + A3) * q2, c = lam + A1 * q2, m = (a + c) / 2, h = (a - c) / 2;
            levels.push_back(0.108256 + m + std::sqrt(h * h + 2 * d2 * d2));
            levels.push_back(0.108256 + m - std::sqrt(h * h + 2 * d2 * d2));
        }
        std::sort(levels.rbegin(), levels.rend());
        const Run r = run(modelOf(oneBox(chuangGaN(QStringLiteral(R"(, "exx": "-0.005")")), QStringLiteral(R"(, "valence": "kp6")"),
                                         QStringLiteral(R"({"type": "bands", "kmax": "1[1/nm]", "points": 2, "states": 6})"))));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        s.select(0);
        double worst = 0;
        QStringList said;
        for (int j = 0; j < 6; ++j) {
            const double got = eV(s, QStringLiteral("qw.Ev%1").arg(2 * j + 1)), pair = eV(s, QStringLiteral("qw.Ev%1").arg(2 * j + 2));
            worst = std::max({worst, std::abs(got - levels[std::size_t(j)]), std::abs(pair - got)});
            said << QStringLiteral("%1 (%2)").arg(got, 0, 'f', 6).arg(levels[std::size_t(j)], 0, 'f', 6);
        }
        qInfo("wurtzite at k = 0: %s", qPrintable(said.join(QStringLiteral(", "))));
        QVERIFY2(worst < 1e-5, qPrintable(QString::number(worst)));
        const double e1 = eV(s, QStringLiteral("qw.Ee1")), e1want = 3.542376 - 4.08 * ezz - 2 * 4.08 * exx + HB2M * std::pow(M_PI / L, 2) / 0.2;
        QVERIFY2(std::abs(e1 - e1want) < 1e-5, qPrintable(QStringLiteral("%1 against %2").arg(e1).arg(e1want)));
        // Parabolic holes: the heavy holes' edge and mass along c alike.
        const Run p = run(modelOf(oneBox(chuangGaN(QStringLiteral(R"(, "exx": "-0.005")")), QString(), QStringLiteral(R"({"type": "states", "states": 1})"))));
        QVERIFY2(p.error.isEmpty(), qPrintable(p.error));
        p.solution->select(0);
        QVERIFY2(std::abs(eV(*p.solution, QStringLiteral("qw.Ehh1")) - levels[0]) < 1e-5, qPrintable(QString::number(eV(*p.solution, QStringLiteral("qw.Ehh1")))));
        // One crystal a stack; wurtzite's k·p the 6x6.
        const Run kp4 = run(modelOf(oneBox(chuangGaN(), QStringLiteral(R"(, "valence": "kp4")"), QStringLiteral(R"({"type": "states", "states": 1})"))));
        QVERIFY2(kp4.error.contains(QLatin1String("wurtzite's is the 6x6")), qPrintable(kp4.error));
        QByteArray mixed = heterostructure(QStringLiteral("GaN"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"), QStringLiteral("5"),
                                           QStringLiteral("10"), QString(), QStringLiteral(R"({"type": "states", "states": 1})"));
        const Run both = run(modelOf(mixed));
        QVERIFY2(both.error.contains(QLatin1String("wurtzite and zinc blende both")), qPrintable(both.error));
    }

    void chuangAndChang97()
    {
        // Lumerical's 6x6 benchmark: Chuang and Chang's strained GaN well
        // (Semicond. Sci. Technol. 12, 252, 1997), 6 nm between Al0.2Ga0.8N, as
        // Lumerical's file has them (GaN εxx -0.48 %; no polarization), their
        // valence subbands along k in the plane - each from the highest's top.
        // The first five (Lumerical's comparison's) within 1.5 meV.
        const QString algan = QStringLiteral(R"("source": "user", "crystal": "wurtzite", "Ec": "3.78701[eV]", "Ev": "0[eV]", "me": "0.22",)"
                                             R"( "A": "-6.54, -0.402, 6.06, -3.032, -3.014, -4.248", "D": "-6.38, 5.18, 8.32, -4.06, -3.88, -5.08",)"
                                             R"( "a1": "-4.6[eV]", "a2": "-11.4[eV]", "Dcr": "-0.0258[eV]", "Dso": "0.0174[eV]", "Ep": "14.1[eV]", "nr": "3.25")");
        const QByteArray json = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "nm",
            "geometry": [{"type": "rectangle", "name": "below", "size": ["10", "10"]},
                         {"type": "rectangle", "name": "well", "position": ["0", "10"], "size": ["10", "6"]},
                         {"type": "rectangle", "name": "above", "position": ["0", "16"], "size": ["10", "10"]}],
            "physics": [{"type": "quantum", "tag": "qw", "valence": "kp6", "h1d": "%3[nm]", "features": [
              {"type": "material", %1, "selection": {"only": ["well"]}},
              {"type": "material", %2, "selection": {"only": ["below", "above"]}}]}],
            "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
          "studies": [{"type": "study", "steps": [{"type": "bands", "kmax": "1.0[1/nm]", "points": 21, "states": 5}]}]})json")
                                    .arg(chuangGaN(QStringLiteral(R"(, "exx": "-0.00484133952")")), algan, slowBuild() ? QStringLiteral("0.2") : QStringLiteral("0.1"))
                                    .toUtf8();
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        // (Their curves' points: k in 1/Å, E in eV from an arbitrary zero.)
        const double ref[5][3][2] = {{{0.0028, 0.01411}, {0.0504, 0.00409}, {0.0918, -0.00984}},
                                     {{0.0030, 0.00422}, {0.0361, -0.00963}, {0.0749, -0.02165}},
                                     {{0.0034, -0.00498}, {0.0530, -0.02740}, {0.0861, -0.04410}},
                                     {{0.0038, -0.01521}, {0.0369, -0.02120}, {0.0700, -0.03878}},
                                     {{0.0030, -0.03610}, {0.0527, -0.04630}, {0.0816, -0.05766}}};
        std::vector<double> ks;
        std::vector<std::array<double, 5>> tops;
        for (int point = 0; point < s.snapshotCount(); ++point) {
            s.select(point);
            ks.push_back(global(s, QStringLiteral("qw.k")) * 1e-10);
            std::array<double, 5> t{};
            for (int i = 0; i < 5; ++i) t[std::size_t(i)] = eV(s, QStringLiteral("qw.Ev%1").arg(2 * i + 1));
            tops.push_back(t);
        }
        auto at = [&](int band, double k) {
            for (std::size_t i = 1; i < ks.size(); ++i)
                if (ks[i] >= k) return tops[i - 1][std::size_t(band)] + (tops[i][std::size_t(band)] - tops[i - 1][std::size_t(band)]) * (k - ks[i - 1]) / (ks[i] - ks[i - 1]);
            return tops.back()[std::size_t(band)];
        };
        const double zero = at(0, ref[0][0][0]);
        double worst = 0;
        for (int band = 0; band < 5; ++band) {
            QStringList said;
            for (int p = 0; p < 3; ++p) {
                const double got = at(band, ref[band][p][0]) - zero, want = ref[band][p][1] - ref[0][0][1];
                worst = std::max(worst, std::abs(got - want));
                said << QStringLiteral("%1 (%2)").arg(got * 1e3, 0, 'f', 1).arg(want * 1e3, 0, 'f', 1);
            }
            qInfo("subband %d: %s meV", band + 1, qPrintable(said.join(QStringLiteral(", "))));
        }
        QVERIFY2(worst < 1.5e-3, qPrintable(QString::number(worst)));
        // The c-plane isotropic (to this order: L1 - M1 = N1): along [110]
        // (45°) the bands along [100]'s, to 1e-9 eV. (Under the sanitizers
        // left out: the Release jobs run it.)
        if (slowBuild()) return;
        QByteArray diagonal = json;
        diagonal.replace(R"("valence": "kp6")", R"("valence": "kp6", "inplane": "110")");
        const Run d = run(modelOf(diagonal));
        QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
        double apart = 0;
        for (int point = 0; point < s.snapshotCount(); ++point) {
            s.select(point);
            d.solution->select(point);
            for (int i = 1; i <= 10; ++i)
                apart = std::max(apart, std::abs(eV(s, QStringLiteral("qw.Ev%1").arg(i)) - eV(*d.solution, QStringLiteral("qw.Ev%1").arg(i))));
        }
        QVERIFY2(apart < 1e-9, qPrintable(QString::number(apart)));
    }

    void wurtziteInterfaces()
    {
        // A GaN well 3 nm between AlN barriers (on GaN): its highest valence
        // band only falls along k in the plane, to 4 /nm. Burt and Foreman's
        // order of the X-Z term keeps it so; symmetrized, (N2 kz + kz N2)/2, a
        // spurious band rises from 1.6 /nm, 72 meV above the top at 4.
        const QByteArray json = QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "nm",
            "geometry": [{"type": "rectangle", "name": "below", "size": ["10", "5"]},
                         {"type": "rectangle", "name": "well", "position": ["0", "5"], "size": ["10", "3"]},
                         {"type": "rectangle", "name": "above", "position": ["0", "8"], "size": ["10", "5"]}],
            "materials": [{"material": "AlN", "compound": "AlN", "selection": {"only": ["below", "above"]}},
                          {"material": "GaN", "compound": "GaN", "selection": {"only": ["well"]}}],
            "physics": [{"type": "quantum", "tag": "qw", "valence": "kp6", "h1d": "%1[nm]", "substrate": "GaN"}],
            "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
          "studies": [{"type": "study", "steps": [{"type": "bands", "kmax": "4[1/nm]", "points": %2, "states": 2}]}]})json")
                                    .arg(slowBuild() ? QStringLiteral("0.1") : QStringLiteral("0.05"))
                                    .arg(slowBuild() ? 11 : 21)
                                    .toUtf8();
        const Run r = run(modelOf(json));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& s = *r.solution;
        double last = INFINITY, rise = 0;
        for (int point = 0; point < s.snapshotCount(); ++point) {
            s.select(point);
            const double E = eV(s, QStringLiteral("qw.Ev1"));
            rise = std::max(rise, E - last);
            last = E;
        }
        QVERIFY2(rise < 1e-4, qPrintable(QString::number(rise)));
    }

    void nitrides()
    {
        // The library's nitrides (Vurgaftman and Meyer's): Al0.3Ga0.7N's gap
        // at 300 K near the 4.05-4.10 eV measured, In0.2Ga0.8N's near 2.7 (its
        // bowing 1.4 eV), In0.18Al0.82N on GaN's lattice, the spontaneous
        // polarization's bowing (Al0.3: -0.0468 C/m²).
        const std::optional<WurtziteParameters> al = wurtziteParameters(QStringLiteral("AlGaN"), 0.3, 300), in = wurtziteParameters(QStringLiteral("InGaN"), 0.2, 300);
        QVERIFY(al && in);
        qInfo("Al0.3Ga0.7N %.3f eV, In0.2Ga0.8N %.3f eV at 300 K; Al0.3's Psp %.4f C/m²", al->Eg, in->Eg, al->Psp);
        QVERIFY2(std::abs(al->Eg - 4.08) < 0.04 && std::abs(in->Eg - 2.70) < 0.05, qPrintable(QStringLiteral("%1 %2").arg(al->Eg).arg(in->Eg)));
        QVERIFY(std::abs(al->Psp - (-0.090 * 0.3 - 0.034 * 0.7 + 0.019 * 0.21)) < 1e-12);
        const std::optional<WurtziteParameters> inal = wurtziteParameters(QStringLiteral("InAlN"), 0.18, 300), gan = wurtziteParameters(QStringLiteral("GaN"), 0, 300);
        QVERIFY(inal && gan && std::abs(inal->a / gan->a - 1) < 2e-3);
        QVERIFY(!wurtziteParameters(QStringLiteral("AlGaN"), 1.2, 300) && isWurtzite(QStringLiteral("ingan")) && !isWurtzite(QStringLiteral("GaAs")));
        // The library's Ev the valence band's top, unstrained: a relaxed GaN
        // box's heavy holes (its top band, Δcr above 0) at Ev + (A1 + A3)
        // ħ²q²/2m0, q = π/L.
        {
            QByteArray json = oneBox(QStringLiteral(R"("source": "material")"), QStringLiteral(R"(, "valence": "kp6")"),
                                     QStringLiteral(R"({"type": "bands", "kmax": "1[1/nm]", "points": 2, "states": 1})"));
            json.replace(R"("geometry")", R"("materials": [{"material": "GaN", "compound": "GaN", "selection": {"all": true}}], "geometry")");
            const Run r = run(modelOf(json));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            r.solution->select(0);
            const double want = gan->Ev + (gan->A[0] + gan->A[2]) * HB2M * std::pow(M_PI / 10, 2), got = eV(*r.solution, QStringLiteral("qw.Ev1"));
            QVERIFY2(std::abs(got - want) < 1e-5, qPrintable(QStringLiteral("%1 against %2").arg(got).arg(want)));
        }
        // A GaN well's band edge TE (its top band X and Y), an AlN box's TM -
        // its crystal field below 0, its top band Z-like.
        auto edge = [&](const QByteArray& json) {
            const Run r = run(modelOf(json));
            if (!r.error.isEmpty()) {
                qWarning("%s", qPrintable(r.error));
                return std::pair<double, double>(NAN, NAN);
            }
            Solution& s = *r.solution;
            s.select(0);
            const double from = global(s, QStringLiteral("qw.Eg"));
            for (int k = 0; k < s.snapshotCount(); ++k) {
                s.select(k);
                if (global(s, QStringLiteral("qw.Ephoton")) > from + 0.01 * Q) return std::pair(global(s, QStringLiteral("qw.alphaTE")), global(s, QStringLiteral("qw.alphaTM")));
            }
            return std::pair<double, double>(NAN, NAN);
        };
        // (Under the sanitizers lighter: 0.25 nm, 41 values of k, 10 meV apart.)
        const auto [teGaN, tmGaN] = edge(lighter(heterostructure(QStringLiteral("GaN"), QStringLiteral("0"), QStringLiteral("AlGaN"), QStringLiteral("0.3"), QStringLiteral("3"),
                                                                 QStringLiteral("6"), QStringLiteral(R"(, "valence": "kp6", "substrate": "GaN")"),
                                                                 QStringLiteral(R"x({"type": "spectrum", "energies": "linspace(3.4[eV], 3.9[eV], %1)", "states": 2, "linewidth": "10[meV]", "lineshape": "gaussian"})x")
                                                                     .arg(slowBuild() ? 51 : 101))));
        QByteArray aln = lighter(oneBox(QStringLiteral(R"("source": "material")"), QStringLiteral(R"(, "valence": "kp6")"),
                                        QStringLiteral(R"x({"type": "spectrum", "energies": "linspace(6.0[eV], 6.6[eV], %1)", "states": 2, "linewidth": "10[meV]", "lineshape": "gaussian"})x")
                                            .arg(slowBuild() ? 61 : 121)));
        aln.replace(R"("geometry")", R"("materials": [{"material": "AlN", "compound": "AlN", "selection": {"all": true}}], "geometry")");
        const auto [teAlN, tmAlN] = edge(aln);
        qInfo("10 meV above the edge: GaN TE %.0f /cm, TM %.0f; AlN TE %.0f, TM %.0f", teGaN / 100, tmGaN / 100, teAlN / 100, tmAlN / 100);
        QVERIFY2(teGaN > 5 * tmGaN && tmAlN > 5 * teAlN, "GaN's TE, AlN's TM");
    }

    void wurtziteOptics()
    {
        // A GaN box: TM's strength its Kane energy along c's (Ep∥ twice Ep,
        // TM twice and TE as it was); the electron's mass across c its own
        // (0.4 m0 against 0.2: the e1-hh1 step as their reduced masses - the
        // holes parabolic, 1/(A2 + A4) across c, for an exact one).
        auto step = [&](const QString& more, const char* valence = "kp6") {
            // (Under the sanitizers lighter: 0.25 nm, 41 values of k, 2.5 meV apart.)
            const Run r = run(modelOf(lighter(oneBox(chuangGaN(more), QStringLiteral(R"(, "valence": "%1")").arg(QLatin1String(valence)),
                                                     QStringLiteral(R"x({"type": "spectrum", "energies": "linspace(3.50[eV], 3.70[eV], %1)", "states": 2, "linewidth": "0[eV]", "kpoints": %2})x")
                                                         .arg(slowBuild() ? 81 : 201)
                                                         .arg(slowBuild() ? 41 : 61)))));
            if (!r.error.isEmpty()) qWarning("%s", qPrintable(r.error));
            std::array<double, 3> out{NAN, NAN, NAN};   // TE and TM 5 meV above the e1-hh1 and the e1-lh1 edges; the hole's mass
            if (!r.error.isEmpty()) return out;
            Solution& s = *r.solution;
            s.select(0);
            const double hh = global(s, QStringLiteral("qw.Ee1")) - global(s, QStringLiteral("qw.Ehh1")) - 0;
            for (int k = 0; k < s.snapshotCount(); ++k) {
                s.select(k);
                const double E = global(s, QStringLiteral("qw.Ephoton"));
                if (std::isnan(out[0]) && E > hh + 0.005 * Q) out[0] = global(s, QStringLiteral("qw.alphaTE"));
                if (E > hh + 0.06 * Q) {
                    out[1] = global(s, QStringLiteral("qw.alphaTM"));
                    break;
                }
            }
            const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("their masses in the plane near it ([0-9.e]+)")).match(s.log.join(QLatin1Char('\n')));
            out[2] = m.hasMatch() ? m.captured(1).toDouble() : NAN;
            return out;
        };
        const auto base = step(QString()), ep = step(QStringLiteral(R"(, "Epz": "28[eV]")"));
        const auto light = step(QString(), "parabolic"), heavy = step(QStringLiteral(R"(, "mep": "0.4")"), "parabolic");
        qInfo("TE %.0f, TM %.0f /cm; Ep∥ twice: %.0f, %.0f; parabolic holes, TE %.0f, me⊥ 0.4: %.0f", base[0] / 100, base[1] / 100, ep[0] / 100,
              ep[1] / 100, light[0] / 100, heavy[0] / 100);
        QVERIFY2(base[1] > 0 && std::abs(ep[1] / base[1] - 2) < 1e-6 && std::abs(ep[0] / base[0] - 1) < 1e-9,
                 qPrintable(QStringLiteral("%1 %2").arg(ep[1] / base[1]).arg(ep[0] / base[0])));
        const double mh = -1 / (-0.91 - 2.83), ratio = (1 / (1 / 0.4 + 1 / mh)) / (1 / (1 / 0.2 + 1 / mh));
        QVERIFY2(std::abs(heavy[0] / light[0] / ratio - 1) < 1e-3, qPrintable(QStringLiteral("%1 against %2").arg(heavy[0] / light[0]).arg(ratio)));
    }

    void polarizationField()
    {
        // A GaN well 3 nm between Al0.3Ga0.7N barriers 6 nm each, on GaN
        // (the barriers stretched: their piezoelectric part too). Periodic:
        // the well's field the closed form's, (Pb - Pw) Lb/(εw Lb + εb Lw)/ε0
        // with Lb both barriers'; no field at its ends: (Pb - Pw)/ε0εw - to
        // 1e-6. The metal face's pushes the electron up the axis (its
        // interface there positive), the nitrogen face's down.
        const std::optional<WurtziteParameters> w = wurtziteParameters(QStringLiteral("GaN"), 0, 300), b = wurtziteParameters(QStringLiteral("AlGaN"), 0.3, 300);
        QVERIFY(w && b);
        const double exx = (w->a - b->a) / b->a, ezz = -2 * b->C13 / b->C33 * exx;
        const double Pw = w->Psp, Pb = b->Psp + 2 * b->e31 * exx + b->e33 * ezz, Lw = 3, Lb = 12;
        const double eps0 = 8.8541878128e-12;
        const double periodic = (Pb - Pw) * Lb / (w->epsr * Lb + b->epsr * Lw) / eps0, ends = (Pb - Pw) / (eps0 * w->epsr);
        auto measured = [&](const char* how, const char* face, const char* step) {
            return run(modelOf(heterostructure(QStringLiteral("GaN"), QStringLiteral("0"), QStringLiteral("AlGaN"), QStringLiteral("0.3"), QStringLiteral("3"),
                                               QStringLiteral("6"),
                                               QStringLiteral(R"(, "substrate": "GaN", "polarization": "%1", "polarity": "%2", "h1d": "%3[nm]")")
                                                   .arg(QLatin1String(how), QLatin1String(face), slowBuild() ? QStringLiteral("0.2") : QStringLiteral("0.05")),
                                               QString::fromLatin1(step))));
        };
        const char* spectrum = R"x({"type": "spectrum", "energies": "linspace(3.3[eV], 3.4[eV], 2)", "states": 1, "linewidth": "10[meV]"})x";
        for (const char* how : {"periodic", "ends"}) {
            const Run r = measured(how, "metal", spectrum);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            r.solution->select(0);
            const double Fw = global(*r.solution, QStringLiteral("qw.Fw")), want = std::abs(!std::strcmp(how, "periodic") ? periodic : ends);
            qInfo("%s: the well's field %.4g V/m (%.4g expected); %s", how, Fw, want,
                  qPrintable(r.solution->log.filter(QStringLiteral("polarization's field")).join(QLatin1Char(' '))));
            QVERIFY2(std::abs(Fw / want - 1) < 1e-6, qPrintable(QString::number(Fw / want - 1)));
        }
        const char* states = R"({"type": "states", "states": 1})";
        const Run up = measured("periodic", "metal", states), down = measured("periodic", "nitrogen", states), none = measured("none", "metal", states);
        QVERIFY2(up.error.isEmpty() && down.error.isEmpty() && none.error.isEmpty(), qPrintable(up.error + down.error + none.error));
        for (const Run* r : {&up, &down, &none}) r->solution->select(0);
        const double middle = global(*none.solution, QStringLiteral("qw.pos_e"));
        const double pu = global(*up.solution, QStringLiteral("qw.pos_e")) - middle, pd = global(*down.solution, QStringLiteral("qw.pos_e")) - middle;
        qInfo("the electron's mean moved %.3f nm (metal face), %.3f nm (nitrogen)", pu * 1e9, pd * 1e9);
        QVERIFY2(pu > 0.05e-9 && pd < -0.05e-9 && std::abs(pu + pd) < 1e-12, qPrintable(QStringLiteral("%1 %2").arg(pu).arg(pd)));
    }

    void polarizationScreened()
    {
        // The well of polarizationField with carriers in a spectrum: its
        // field screened - less as the density grows, by less than their
        // sheets' charge q n Lw/ε0εw were they at its two faces (they are
        // spread) -, the e1-hh1 transition moving up (the QCSE undone); the
        // loop converged. Parabolic holes and the 6x6's alike. (Under the
        // sanitizers the parabolic at 0 and 4e19, 0.2 nm.)
        for (const char* valence : {"parabolic", "kp6"}) {
            if (slowBuild() && !std::strcmp(valence, "kp6")) continue;
            double lastF = NAN, lastE = NAN;
            for (const char* density : {"0", "1e19", "4e19"}) {
                if (slowBuild() && !std::strcmp(density, "1e19")) continue;
                const Run r = run(modelOf(heterostructure(
                    QStringLiteral("GaN"), QStringLiteral("0"), QStringLiteral("AlGaN"), QStringLiteral("0.3"), QStringLiteral("3"), QStringLiteral("6"),
                    QStringLiteral(R"(, "substrate": "GaN", "polarization": "periodic", "valence": "%1", "h1d": "%2[nm]")")
                        .arg(QLatin1String(valence), slowBuild() ? QStringLiteral("0.2") : QStringLiteral("0.1")),
                    QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(3.3[eV], 3.9[eV], 3)", "states": 2, "linewidth": "10[meV]", "density": "%1[1/cm^3]", "kpoints": 31})x")
                        .arg(QLatin1String(density)))));
                QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
                Solution& s = *r.solution;
                s.select(0);
                const double F = global(s, QStringLiteral("qw.Fw")), E = global(s, QStringLiteral("qw.Ee1hh1"));
                const QString log = s.log.join(QLatin1Char('\n'));
                qInfo("%s at %s cm⁻³: the well's field %.4g V/m, e1-hh1 %.4f eV; %s", valence, density, F, E / Q,
                      qPrintable(s.log.filter(QStringLiteral("screened")).join(QLatin1Char(' '))));
                if (std::strcmp(density, "0") != 0) {
                    QVERIFY2(log.contains(QLatin1String("screened by the spectrum's carriers")) && !log.contains(QLatin1String("still moves")), qPrintable(log));
                    const double sheet = std::atof(density) * 1e6 * 3e-9, bound = Q * sheet / (8.8541878128e-12 * 10.4);
                    QVERIFY2(F < lastF && lastF - F < bound && E > lastE, qPrintable(QStringLiteral("%1 %2 %3 %4").arg(F).arg(lastF).arg(bound).arg(E)));
                }
                lastF = F;
                lastE = E;
            }
        }
    }

    void hdf5()
    {
        // The HDF5 of Lumerical's projects (data/lumerical_sample.py's,
        // h5py's newest layout): a group of a hundred links (dense: a fractal
        // heap, a B-tree of two levels), a record of seventy attributes
        // (dense too, its heap of indirect blocks), its models' groups by
        // their names, an int8 dataset; not one, said.
        Hdf5File f;
        QString why;
        QVERIFY2(f.open(QStringLiteral(QUCS_TEST_DATA "/lumerical_sample.ldev"), &why), qPrintable(why));
        const std::vector<std::pair<QString, quint64>> objects = f.links(f.find(QStringLiteral("SimObjectRoot/Children/1/Children")));
        QCOMPARE(int(objects.size()), 100);
        const quint64 property = f.find(QStringLiteral("SimObjectRoot/Children/1/Children/1/Children/1"));
        QVERIFY(property != 0);
        const auto attrs = f.attributes(property);
        QVERIFY(attrs.count(QStringLiteral("Material Properties UID")));
        const quint64 record = f.find(QStringLiteral("Custom/1/") + attrs.at(QStringLiteral("Material Properties UID")).string(), property);
        const auto rec = f.attributes(record);
        QCOMPARE(int(rec.size()), 70);
        QCOMPARE(rec.at(QStringLiteral("spare 59")).number(), 59.0);
        QCOMPARE(rec.at(QStringLiteral("epsilonr")).number(), 12.9);
        QCOMPARE(rec.at(QStringLiteral("name")).string(), QStringLiteral("GaAs (sample)"));
        QCOMPARE(int(rec.at(QStringLiteral("mEpsrDiagonal")).numbers.size()), 3);
        QCOMPARE(rec.at(QStringLiteral("last modified")).number(), 1415223023.0);
        const std::optional<Hdf5File::Value> blob = f.dataset(f.find(QStringLiteral("MqwSimObject/Properties")));
        QVERIFY(blob && blob->raw.size() > 500 && blob->numbers.size() == std::size_t(blob->raw.size()));
        Hdf5File none;
        QVERIFY(!none.open(QStringLiteral(QUCS_TEST_DATA "/lumerical_sample.ldf"), &why) && why.contains(QLatin1String("not an HDF5 file")));
    }

    // (Four functions, each under QtTest's limit: as one, some 1,460 s on
    // CI's sanitized Linux, its limit 900.)
    void lumericalProjects()
    {
        // data/lumerical_sample.ldev (zinc blende, 4x4): its layers a stack,
        // each its material's gap at 300 K (Varshni's) over its offset, its
        // masses of γ, the well strained (εxx -1 %: ac, av, b, C11, C12); the
        // field, the grid step; k to 0.2 π/a (the well's), the spectrum's
        // energies of its frequencies, its density per the wells (22/6 of
        // 1e18), its broadening half of 13.2 meV - and it solves.
        const LumericalImport zb = importLumericalMqw(QStringLiteral(QUCS_TEST_DATA "/lumerical_sample.ldev"));
        QVERIFY2(zb.error.isEmpty(), qPrintable(zb.error));
        qInfo("%s", qPrintable(zb.notes.join(QLatin1Char('\n'))));
        QVERIFY(zb.notes.front().contains(QLatin1String("3 layers")) && zb.notes.front().contains(QStringLiteral("the k·p 4x4")));
        const QJsonObject c = zb.model.value("components").toArray().at(0).toObject();
        const QJsonArray geometry = c.value("geometry").toArray();
        QCOMPARE(geometry.size(), 3);
        QCOMPARE(geometry.at(1).toObject().value("position").toArray().at(1).toString(), QStringLiteral("8"));
        QCOMPARE(geometry.at(1).toObject().value("size").toArray().at(1).toString(), QStringLiteral("6"));
        const QJsonObject physics = c.value("physics").toArray().at(0).toObject();
        QCOMPARE(physics.value("valence").toString(), QStringLiteral("kp4"));
        QCOMPARE(physics.value("F").toString(), QStringLiteral("5000000[V/m]"));
        QCOMPARE(physics.value("h1d").toString(), QStringLiteral("0.2[nm]"));
        const QJsonObject well = physics.value("features").toArray().at(1).toObject();
        const double eg = 1.30 - 4.5e-4 * 300 * 300 / (300 + 180);
        auto value = [](const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString().section(QLatin1Char('['), 0, 0).toDouble(); };
        QVERIFY(std::abs(value(well, "Ec") - (-0.75 + eg)) < 1e-9 && std::abs(value(well, "Ev") + 0.75) < 1e-12);
        QVERIFY(std::abs(value(well, "mhh") - 1 / (7.6 - 4.8)) < 1e-9 && std::abs(value(well, "mlh") - 1 / (7.6 + 4.8)) < 1e-9);
        QVERIFY(std::abs(value(well, "exx") + 0.01) < 1e-15 && std::abs(value(well, "C12") - 5.45e10) < 1);
        QCOMPARE(well.value("me").toString(), QStringLiteral("0.058"));
        const QJsonArray studies = zb.model.value("studies").toArray();
        const QJsonObject bands = studies.at(0).toObject().value("steps").toArray().at(0).toObject();
        QVERIFY(std::abs(value(bands, "kmax") - 0.2 * M_PI / 5.71e-10) < 1);
        QCOMPARE(bands.value("points").toInt(), 11);
        QCOMPARE(bands.value("states").toInt(), 5);   // (its 10 eigenvalues' pairs)
        // Every bound state too, and each drawn (the MQW benchmarks' round 2:
        // three wells' 9 subbands, where 20 states took 7).
        QVERIFY(bands.value("bound").toBool());
        const QJsonObject bandsPlot = zb.model.value("results").toObject().value("plots").toArray().at(0).toObject();
        QCOMPARE(bandsPlot.value("study").toString(), QStringLiteral("bands"));
        QCOMPARE(bandsPlot.value("plots").toArray().size(), 1);
        QCOMPARE(bandsPlot.value("plots").toArray().at(0).toObject().value("expression").toString(), QStringLiteral("qw.Ev*"));
        const QJsonObject spectrum = studies.at(1).toObject().value("steps").toArray().at(0).toObject();
        QVERIFY(std::abs(value(spectrum, "density") / (1e24 * 22 / 6) - 1) < 1e-9 && std::abs(value(spectrum, "linewidth") - 0.0066) < 1e-12);
        QVERIFY2(spectrum.value("energies").toString().startsWith(QStringLiteral("linspace(%1[eV], ").arg(6.62607015e-34 * 3.3e14 / Q, 0, 'g', 10)),
                 qPrintable(spectrum.value("energies").toString()));
        // Its first study solved: the strained well's heavy holes above its
        // own Ev (compressed).
        const Run r = run(modelOf(QJsonDocument(zb.model).toJson()));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        r.solution->select(0);
        const double hh = eV(*r.solution, QStringLiteral("qw.Ev1"));
        QVERIFY2(hh > -0.75 && hh < -0.70, qPrintable(QString::number(hh)));
        // (Its bound states counted and said: at least the 5 pairs asked.)
        QVERIFY2(r.solution->log.join(QLatin1Char('\n')).contains(QLatin1String("bound in the wells at k = 0")), qPrintable(r.solution->log.join(QLatin1Char('\n'))));
        QVERIFY(r.solution->global(QStringLiteral("qw.Ev10")).has_value());
    }

    // data/lumerical_wurtzite.ldev (6x6): its layers wurtzite - A, D, Δ's,
    // the electron's masses, the well's strain with C13, C33 - and it solves.
    void lumericalWurtzite()
    {
        auto value = [](const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString().section(QLatin1Char('['), 0, 0).toDouble(); };
        const LumericalImport wz = importLumericalMqw(QStringLiteral(QUCS_TEST_DATA "/lumerical_wurtzite.ldev"));
        QVERIFY2(wz.error.isEmpty(), qPrintable(wz.error));
        const QJsonObject wphysics = wz.model.value("components").toArray().at(0).toObject().value("physics").toArray().at(0).toObject();
        QCOMPARE(wphysics.value("valence").toString(), QStringLiteral("kp6"));
        const QJsonObject gan = wphysics.value("features").toArray().at(1).toObject();
        QCOMPARE(gan.value("crystal").toString(), QStringLiteral("wurtzite"));
        QCOMPARE(gan.value("A").toString(), QStringLiteral("-6.56, -0.91, 5.65, -2.83, -3.13, -4.86"));
        QCOMPARE(gan.value("exx").toString(), QStringLiteral("-0.00484133952"));
        QVERIFY(std::abs(value(gan, "C13") / value(gan, "C33") - 1.06 / 3.98) < 1e-12);
        // (Under the sanitizers its bands without the bound ones: 9 pairs
        // there, 5 asked - 117 s against 49, over a third of QtTest's
        // limit on CI's.)
        QByteArray json = QJsonDocument(wz.model).toJson(QJsonDocument::Compact);
        QVERIFY(json.contains(R"("bound":true)"));
        if (slowBuild()) json.replace(R"("bound":true)", R"("bound":false)");
        const Run w = run(modelOf(json));
        QVERIFY2(w.error.isEmpty(), qPrintable(w.error));
    }

    // Lumerical's own band offsets ("override band offset" 0), the layer
    // table's not: the valence band's steps its last run stored - 0.1095 eV
    // up in the well, the field's slope taken out -; its stack padded 15 nm
    // each side; its photons in eV ("freq type" 2); its excitons on, so no
    // carriers; its subbands 2 and 2, its cutoffs (hard walls), its window
    // and stack as Lumerical's.
    void lumericalOffsets()
    {
        auto value = [](const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString().section(QLatin1Char('['), 0, 0).toDouble(); };
        const LumericalImport mod = importLumericalMqw(QStringLiteral(QUCS_TEST_DATA "/lumerical_mqw_offsets.ldev"));
        QVERIFY2(mod.error.isEmpty(), qPrintable(mod.error));
        const QString said = mod.notes.join(QLatin1Char('\n'));
        QVERIFY2(said.contains(QLatin1String("its band diagram")) && said.contains(QLatin1String("15 nm")) && said.contains(QLatin1String("excitons on")),
                 qPrintable(said));
        const QJsonObject mc = mod.model.value("components").toArray().at(0).toObject();
        const QJsonArray mg = mc.value("geometry").toArray();
        QCOMPARE(mg.at(0).toObject().value("size").toArray().at(1).toString(), QStringLiteral("23"));
        QCOMPARE(mg.at(1).toObject().value("position").toArray().at(1).toString(), QStringLiteral("23"));
        QCOMPARE(mg.at(2).toObject().value("size").toArray().at(1).toString(), QStringLiteral("23"));
        const QJsonArray mf = mc.value("physics").toArray().at(0).toObject().value("features").toArray();
        QVERIFY(std::abs(value(mf.at(0).toObject(), "Ev") + 0.80) < 1e-12);
        QVERIFY2(std::abs(value(mf.at(1).toObject(), "Ev") - (-0.80 + 0.1095)) < 1e-9, qPrintable(mf.at(1).toObject().value("Ev").toString()));
        QVERIFY(std::abs(value(mf.at(2).toObject(), "Ev") + 0.80) < 1e-9);
        const QJsonObject ms = mod.model.value("studies").toArray().at(1).toObject().value("steps").toArray().at(0).toObject();
        QCOMPARE(ms.value("energies").toString(), QStringLiteral("linspace(1.2[eV], 1.5[eV], 31)"));
        QVERIFY(!ms.contains("density") && ms.value("excitons").toBool());
        QCOMPARE(ms.value("cstates").toInt(), 2);
        QCOMPARE(ms.value("vstates").toInt(), 2);
        QCOMPARE(ms.value("cutoff_c").toString(), QStringLiteral("0.1"));
        QCOMPARE(ms.value("cutoff_v").toString(), QStringLiteral("0.1"));
        QCOMPARE(ms.value("broaden").toString(), QStringLiteral("window"));
        QCOMPARE(ms.value("normalize").toString(), QStringLiteral("stack"));
        {
            // (Its spectrum's study solved: the subbands chosen, broadened in
            // the window, per the stack.)
            // (Sanitized, coarser still: 486 s there at 0.25 nm and 41 values
            // of k - Eigen's dense solves unoptimized -, some 1,200 on CI's.)
            QByteArray json = lighter(QJsonDocument(mod.model).toJson());
            if (slowBuild()) json.replace(R"("h1d": "0.25[nm]")", R"("h1d": "0.5[nm]")").replace(R"("kpoints": 41)", R"("kpoints": 21)");
            const Model m = modelOf(json);
            const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
            const GeometryBuild g = buildGeometry(m, *ps);
            QVERIFY(g.ok());
            const StudyRun sr = runStudy(m, ps, g.topology, nullptr, QStringLiteral("gain"));
            QVERIFY2(sr.ok(), qPrintable(sr.error));
            Solution& s = *sr.solutions.back();
            const QString log = s.log.join(QLatin1Char('\n'));
            // (Its shallow well's electrons, 0.065 eV deep, below the field's
            // triangle at the stack's end: those against the wall left out.)
            QVERIFY2(log.contains(QLatin1String("2 valence (Kramers pairs)")) && log.contains(QLatin1String("left out (at a wall")), qPrintable(log));
            s.select(0);
            QVERIFY(std::abs(global(s, QStringLiteral("qw.Lnorm")) - 52e-9) < 1e-15 && std::abs(global(s, QStringLiteral("qw.Lstack")) - 52e-9) < 1e-15);
        }
    }

    // None stored: its materials' work functions (χ = W - Eg/2 - kT/2
    // ln(Nc/Nv), Nc/Nv of their masses); its photons as wavelengths ("freq
    // type" 1); carriers kept; PMLs - its cutoffs not taken, said. Not a
    // project: said.
    void lumericalWorkFunctions()
    {
        auto value = [](const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString().section(QLatin1Char('['), 0, 0).toDouble(); };
        const LumericalImport wf = importLumericalMqw(QStringLiteral(QUCS_TEST_DATA "/lumerical_mqw_workfunctions.ldev"));
        QVERIFY2(wf.error.isEmpty(), qPrintable(wf.error));
        const QJsonArray wfeatures = wf.model.value("components").toArray().at(0).toObject().value("physics").toArray().at(0).toObject().value("features").toArray();
        const double kT = 8.617333262e-5 * 300;
        auto ev = [&](double W, double Eg, double me, double mh) { return -(W + Eg / 2 - kT / 2 * 1.5 * std::log(me / mh)); };
        const double egGaAs = 1.519 - 5.405e-4 * 300 * 300 / (300 + 204), egInGaAs = 1.30 - 4.5e-4 * 300 * 300 / (300 + 180);
        QVERIFY(std::abs(value(wfeatures.at(0).toObject(), "Ev") - ev(4.83, egGaAs, 0.067, 0.5)) < 1e-8);
        QVERIFY(std::abs(value(wfeatures.at(1).toObject(), "Ev") - ev(4.75, egInGaAs, 0.058, 0.45)) < 1e-8);
        QVERIFY(std::abs(value(wfeatures.at(1).toObject(), "Ec") - ev(4.75, egInGaAs, 0.058, 0.45) - egInGaAs) < 1e-8);
        const QJsonObject ws = wf.model.value("studies").toArray().at(1).toObject().value("steps").toArray().at(0).toObject();
        QCOMPARE(ws.value("energies").toString(), QStringLiteral("linspace(0.85[um], 1[um], 31)"));
        QVERIFY(std::abs(value(ws, "density") / (2e24 * 22 / 6) - 1) < 1e-9);
        QVERIFY(!ws.contains("cutoff_c") && !ws.contains("cutoff_v"));
        QVERIFY2(wf.notes.join(QLatin1Char(' ')).contains(QLatin1String("work functions")) && wf.notes.join(QLatin1Char(' ')).contains(QLatin1String("PMLs")),
                 qPrintable(wf.notes.join(QLatin1Char('\n'))));
        // Not a project: said.
        QVERIFY(!importLumericalMqw(QStringLiteral(QUCS_TEST_DATA "/lumerical_sample.ldf")).error.isEmpty());
        QVERIFY(importLumericalMqw(QStringLiteral(QUCS_TEST_DATA "/klayout_sample.oas")).error.contains(QLatin1String("not an HDF5 file")));
    }
};

QTEST_MAIN(TestFemNitrides)
#include "test_fem_nitrides.moc"
