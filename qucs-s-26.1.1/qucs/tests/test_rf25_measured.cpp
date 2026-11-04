/*
 * test_rf25_measured.cpp - the RF 2.5D solver held to published
 *                          measurements, each cited: a structure as it was
 *                          made and measured, solved as the measurement
 *                          was taken, and how far the two are apart said
 *                          and held within what the measurement's own
 *                          uncertainties and the mesh allow
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <QtTest>

#include "rf25/rf25_deembed.h"
#include "rf25/rf25_fullwave.h"
#include "rf25/rf25_mesh.h"

#include <cmath>

namespace rf = qucs_s::rf25;

namespace {

constexpr double kMu0 = 4e-7 * M_PI;
const double kC0 = 1.0 / std::sqrt(kMu0 * rf::kEps0);

/// Whether this build is one whose dense solves are many times slower - a
/// sanitized one, as CI's Linux job is (QUCS_TEST_SLOW: its path in any).
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

} // namespace

class TestRf25Measured : public QObject
{
    Q_OBJECT

private slots:
    // A coplanar waveguide on alumina, as G. Gold, K. Lomakin, K. Helmreich
    // and U. Arz made and measured it: "High-Frequency Modeling of Coplanar
    // Waveguides Including Surface Roughness", Adv. Radio Sci. 17, 51-57
    // (2019), doi:10.5194/ars-17-51-2019 (CC BY 4.0). Its Table 1 and
    // section 2: a 50 µm signal, 25 µm gaps and 270 µm grounds of
    // electroplated metal, taken as 6.5 µm thick (it varies across the
    // wafer) and 36 MS/m, on 500 µm of Al2O3 of 9.7 and tan d 1.25e-4 (a
    // split-cylinder resonator's, 8 to 22 GHz); measured on a metal chuck -
    // a ground under the substrate - its propagation constant by a
    // multiline calibration. Its effective permittivity and attenuation as
    // its Figs. 3 and 2 measured them, read from the vector figures' own
    // paths at their axes' ticks, each the mean within 0.5 GHz.
    //
    // Solved as it was measured: the strips of the line's own coplanar feed
    // - the shorter, the longer and the third that checks them - give its
    // propagation constant, as a multiline calibration does; meshed as the
    // tab meshes it, three cells across the signal, none over 100 µm, and
    // the sides facing its gaps graded to a quarter of them. Held to the
    // measurement and to the smooth line's own loss, a 2D eddy-current
    // solve of its cross-section (docs/feature_gaps/2026-11-04-claude-rf25-
    // fullwave-gaps/eddy.py: R 731.4, 1267 and 1795 Ohm/m, its chuck a
    // perfect plane; Wheeler's rule on its own inductance comes within
    // 3.6 % at 10 GHz and 1.6 % at 13 THz, its sharp corners' share
    // closing as the skin depth does), whose attenuation is
    // R sqrt(eps_eff)/(2 c L) and the substrate's loss beside it:
    // - eps_eff within 2 %: 0.4 to 0.6 % under (1.1 to 1.6 % under on the
    //   edges' one halving), and the paper puts its own models' offset down
    //   to the substrate's permittivity, the metal's thickness and
    //   roughness.
    // - alpha within 12 % of the smooth line's: 2 % under at 10 GHz, 6 and
    //   10 % over at 30 and 60. A metal solved as its two faces carries no
    //   current down its walls, so its narrow edge cells take the crowding
    //   in only as the frequency rises - its inductance 6 % over, its loss
    //   the more over the higher the frequency (on the edges' one halving
    //   the grounds' 45 µm outermost cells left it 20 to 14 % under).
    // - alpha within 8 % of the measurement: 0.2 % under to 5 % over. The
    //   smooth line is itself 1.4 % over the measurement at 10 GHz, 0.2 %
    //   under at 30 and 4.5 % at 60 - the roughness the paper measured, 63.5
    //   nm rms, adds a few per cent there.
    // - how it grows - alpha at 30 and 60 GHz over alpha at 10 - within 8 %
    //   of the measured: faster than the root of the frequency a metal's
    //   loss alone gives (1.73 and 2.45), the bend the paper's quasi-TEM
    //   model missed and its model of radiation and dispersion found (its
    //   Figs. 2 and 4). Here 5 % over it, the faces' crowding rising with
    //   the frequency as above; the smooth line's 1.75 and 2.49 lie 2 and
    //   6 % under.
    void aCoplanarLineOnAluminaIsTheMeasuredOne()
    {
        if (slowBuild())
            QSKIP("its strips, 4.1 to 8.2 mm long over 500 µm of alumina: many minutes under the sanitizers (held in "
                  "Release builds - CI's macOS job)");
        rf::Stack stack;
        rf::Layer ground;
        ground.kind = rf::Layer::Ground;
        ground.name = QStringLiteral("GND");
        ground.tag = QStringLiteral("gnd");
        rf::Layer sub;
        sub.kind = rf::Layer::Substrate;
        sub.name = QStringLiteral("AL2O3");
        sub.tag = QStringLiteral("al2o3");
        sub.thickness = 500e-6;
        sub.epsr = 9.7;
        sub.tand = 1.25e-4;
        rf::Layer metal;
        metal.kind = rf::Layer::Metal;
        metal.name = QStringLiteral("M1");
        metal.tag = QStringLiteral("m1");
        metal.thickness = 6.5e-6;
        metal.sigma = 36e6;
        metal.twoSheets = true;
        stack.layers << ground << sub << metal;
        rf::Feed feed;
        feed.metal = QStringLiteral("M1");
        feed.width = 50;
        feed.coplanar = true;
        feed.gap[0] = feed.gap[1] = 25;
        feed.ground[0] = feed.ground[1] = 270;
        // (Finer, a test's time is past: four cells across and 50 µm took
        // a minute at 10 GHz alone on the edges' one halving, and its
        // compressed solve did not converge at 60 GHz in GMRES's 3000
        // iterations on the longer strip, 27804 unknowns.)
        rf::MeshSettings how;
        how.cellsAcross = 3;
        how.maximum = 100e-6;
        how.gaps = true;   // (as the tab meshes either study)
        // Hz, eps_eff, Np/m: as measured; and the smooth line's R (Ohm/m)
        // and L (H/m) by the 2D eddy-current solve.
        const struct {
            double frequency, eps, alpha, r, l;
        } measured[] = {{10e9, 4.934, 7.66, 731.44, 3.4995e-7},
                        {30e9, 4.874, 13.63, 1267.33, 3.4506e-7},
                        {60e9, 4.868, 20.33, 1795.37, 3.4309e-7}};
        QList<double> fs;
        for (const auto& m : measured) fs << m.frequency;
        const rf::Calibration cal = rf::calibrate(stack, how, 0, feed, fs);
        QVERIFY2(cal.ok(), qPrintable(cal.problems.join(" | ")));
        QList<double> eps, alpha, smooth;
        for (int k = 0; k < fs.size(); ++k) {
            eps << std::pow(cal.gamma.at(k).imag() * kC0 / (2 * M_PI * fs.at(k)), 2);
            alpha << cal.gamma.at(k).real();
            // The smooth line's: its metal's R sqrt(eps_eff)/(2 c L), at the
            // measured eps_eff, and its substrate's loss.
            const double e = measured[k].eps, w = 2 * M_PI * fs.at(k);
            const double dielectric = w / (2 * kC0) * sub.epsr / std::sqrt(e) * (e - 1) / (sub.epsr - 1) * sub.tand;
            smooth << measured[k].r * std::sqrt(e) / (2 * kC0 * measured[k].l) + dielectric;
            qWarning("%g GHz: eps_eff %.4f (measured %.3f, %+.2f %%), alpha %.2f Np/m (measured %.2f, %+.1f %%; the smooth "
                     "line's %.2f, %+.1f %%); the strips agree to %.2g",
                     fs.at(k) / 1e9, eps.at(k), measured[k].eps, 100 * (eps.at(k) / measured[k].eps - 1), alpha.at(k),
                     measured[k].alpha, 100 * (alpha.at(k) / measured[k].alpha - 1), smooth.at(k),
                     100 * (alpha.at(k) / smooth.at(k) - 1), cal.residual.at(k));
        }
        for (int k = 0; k < fs.size(); ++k) {
            const QString at = QStringLiteral(" at %1 GHz").arg(fs.at(k) / 1e9);
            QVERIFY2(cal.residual.at(k) <= rf::kStripsAgree,
                     qPrintable(QStringLiteral("the strips agree to %1").arg(cal.residual.at(k)) + at));
            QVERIFY2(std::abs(eps.at(k) / measured[k].eps - 1) < 0.02,
                     qPrintable(QStringLiteral("eps_eff %1, measured %2").arg(eps.at(k)).arg(measured[k].eps) + at));
            QVERIFY2(std::abs(alpha.at(k) / smooth.at(k) - 1) < 0.12,
                     qPrintable(QStringLiteral("alpha %1 Np/m, the smooth line's %2").arg(alpha.at(k)).arg(smooth.at(k)) + at));
            QVERIFY2(std::abs(alpha.at(k) / measured[k].alpha - 1) < 0.08,
                     qPrintable(QStringLiteral("alpha %1 Np/m, measured %2").arg(alpha.at(k)).arg(measured[k].alpha) + at));
        }
        for (int k = 1; k < fs.size(); ++k) {
            const double solved = alpha.at(k) / alpha.at(0), was = measured[k].alpha / measured[0].alpha;
            QVERIFY2(solved > std::sqrt(fs.at(k) / fs.at(0)) && std::abs(solved / was - 1) < 0.08,
                     qPrintable(QStringLiteral("alpha at %1 GHz %2 times that at 10, measured %3 times")
                                    .arg(fs.at(k) / 1e9).arg(solved).arg(was)));
        }
    }
};

QTEST_MAIN(TestRf25Measured)
#include "test_rf25_measured.moc"
