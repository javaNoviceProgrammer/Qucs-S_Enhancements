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
    // propagation constant, as a multiline calibration does. Held to the
    // measurement:
    // - eps_eff within 2 %. On this mesh - three cells across the signal,
    //   none over 100 µm - it is 1.1 to 1.6 % under; finer ones come closer
    //   (at 10 GHz 4.854 here, 4.880 at four cells and 50 µm, 4.890 at six;
    //   measured 4.934), and the paper puts its own models' offset down to
    //   the substrate's permittivity, the metal's thickness and roughness.
    // - alpha under the measurement - a smooth metal's, which the measured
    //   one was not - by no more than a quarter. On this mesh the current
    //   crowding at the conductors' edges is not resolved and it is some 18 %
    //   under (at 10 GHz 6.23 Np/m here, 6.94 at four cells and 50 µm, 6.93
    //   at six; a coplanar line's conductor loss by the incremental
    //   inductance rule, its grounds wide, 7.35); the roughness the paper
    //   measured, 63.5 nm rms, adds a few percent at 60 GHz.
    // - how it grows - alpha at 30 and 60 GHz over alpha at 10 - within 5 %
    //   of the measured: faster than the root of the frequency a metal's
    //   loss alone gives (1.73 and 2.45), the bend the paper's quasi-TEM
    //   model missed and its model of radiation and dispersion found (its
    //   Figs. 2 and 4).
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
        // a minute at 10 GHz alone, and its compressed solve did not
        // converge at 60 GHz in GMRES's 3000 iterations on the longer
        // strip, 27804 unknowns.)
        rf::MeshSettings how;
        how.cellsAcross = 3;
        how.maximum = 100e-6;
        // Hz, eps_eff, Np/m: as measured.
        const struct {
            double frequency, eps, alpha;
        } measured[] = {{10e9, 4.934, 7.66}, {30e9, 4.874, 13.63}, {60e9, 4.868, 20.33}};
        QList<double> fs;
        for (const auto& m : measured) fs << m.frequency;
        const rf::Calibration cal = rf::calibrate(stack, how, 0, feed, fs);
        QVERIFY2(cal.ok(), qPrintable(cal.problems.join(" | ")));
        QList<double> eps, alpha;
        for (int k = 0; k < fs.size(); ++k) {
            eps << std::pow(cal.gamma.at(k).imag() * kC0 / (2 * M_PI * fs.at(k)), 2);
            alpha << cal.gamma.at(k).real();
            qWarning("%g GHz: eps_eff %.4f (measured %.3f, %+.2f %%), alpha %.2f Np/m (measured %.2f, %+.1f %%); the strips "
                     "agree to %.2g",
                     fs.at(k) / 1e9, eps.at(k), measured[k].eps, 100 * (eps.at(k) / measured[k].eps - 1), alpha.at(k),
                     measured[k].alpha, 100 * (alpha.at(k) / measured[k].alpha - 1), cal.residual.at(k));
        }
        for (int k = 0; k < fs.size(); ++k) {
            const QString at = QStringLiteral(" at %1 GHz").arg(fs.at(k) / 1e9);
            QVERIFY2(cal.residual.at(k) <= rf::kStripsAgree,
                     qPrintable(QStringLiteral("the strips agree to %1").arg(cal.residual.at(k)) + at));
            QVERIFY2(std::abs(eps.at(k) / measured[k].eps - 1) < 0.02,
                     qPrintable(QStringLiteral("eps_eff %1, measured %2").arg(eps.at(k)).arg(measured[k].eps) + at));
            QVERIFY2(alpha.at(k) < measured[k].alpha && alpha.at(k) > 0.75 * measured[k].alpha,
                     qPrintable(QStringLiteral("alpha %1 Np/m, measured %2").arg(alpha.at(k)).arg(measured[k].alpha) + at));
        }
        for (int k = 1; k < fs.size(); ++k) {
            const double solved = alpha.at(k) / alpha.at(0), was = measured[k].alpha / measured[0].alpha;
            QVERIFY2(std::abs(solved / was - 1) < 0.05,
                     qPrintable(QStringLiteral("alpha at %1 GHz %2 times that at 10, measured %3 times")
                                    .arg(fs.at(k) / 1e9).arg(solved).arg(was)));
        }
    }
};

QTEST_MAIN(TestRf25Measured)
#include "test_rf25_measured.moc"
