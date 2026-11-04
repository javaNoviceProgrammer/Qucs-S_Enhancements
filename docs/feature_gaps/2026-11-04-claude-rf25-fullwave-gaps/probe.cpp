// The measured alumina line's calibration, its R, L, G, C per metre:
// probe gaps sigma tand cellsAcross maximum_um [faces] [substrate_um] [freqs GHz,...]
#include <QCoreApplication>
#include <QStringList>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include "rf25/rf25_deembed.h"
#include "rf25/rf25_fullwave.h"
#include "rf25/rf25_mesh.h"
namespace rf = qucs_s::rf25;
static double env(const char* name, double fallback)
{
    const char* v = std::getenv(name);
    return v != nullptr ? std::atof(v) : fallback;
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const bool gaps = argc > 1 && std::atoi(argv[1]) != 0;
    const double sigma = argc > 2 ? std::atof(argv[2]) : 36e6;
    const double tand = argc > 3 ? std::atof(argv[3]) : 0;
    const int across = argc > 4 ? std::atoi(argv[4]) : 3;
    const double maximum = argc > 5 ? std::atof(argv[5]) : 100;
    const bool faces = argc > 6 ? std::atoi(argv[6]) != 0 : true;
    const double sub_um = argc > 7 ? std::atof(argv[7]) : 500;
    QList<double> fs = {10e9, 30e9, 60e9};
    if (argc > 8) {
        fs.clear();
        for (const QString& s : QString::fromLatin1(argv[8]).split(',')) fs << s.toDouble() * 1e9;
    }
    rf::Stack stack;
    rf::Layer ground;
    ground.kind = rf::Layer::Ground;
    ground.name = "GND";
    ground.tag = "gnd";
    rf::Layer sub;
    sub.kind = rf::Layer::Substrate;
    sub.name = "AL2O3";
    sub.tag = "al2o3";
    sub.thickness = sub_um * 1e-6;
    sub.epsr = env("PROBE_EPS", 9.7);
    sub.tand = tand;
    rf::Layer metal;
    metal.kind = rf::Layer::Metal;
    metal.name = "M1";
    metal.tag = "m1";
    metal.thickness = env("PROBE_T", 6.5) * 1e-6;
    metal.sigma = sigma;
    metal.twoSheets = faces;
    stack.layers << ground << sub << metal;
    rf::Feed feed;
    feed.metal = "M1";
    feed.width = env("PROBE_S", 50);
    feed.coplanar = true;
    feed.gap[0] = feed.gap[1] = env("PROBE_G", 25);
    feed.ground[0] = feed.ground[1] = env("PROBE_WG", 270);
    rf::MeshSettings how;
    how.cellsAcross = across;
    how.maximum = maximum * 1e-6;
    how.gaps = gaps;
    rf::FullWaveControl control;
    const bool dense = std::getenv("PROBE_DENSE") != nullptr;
    if (dense) {
        control.matrix = rf::FullWaveControl::Matrix::Dense;
        control.mostUnknowns = 60000;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const rf::Calibration cal = rf::calibrate(stack, how, 0, feed, fs, 1e-6, dense ? &control : nullptr);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!cal.ok()) {
        std::printf("not ok: %s\n", qPrintable(cal.problems.join(" | ")));
        return 1;
    }
    for (const QString& l : cal.lines) std::printf("  | %s\n", qPrintable(l));
    constexpr double mu0 = 4e-7 * M_PI;
    const double c0 = 1.0 / std::sqrt(mu0 * rf::kEps0);
    std::printf("gaps %d sigma %g tand %g across %d max %g faces %d sub %g: %.0f s\n", gaps, sigma, tand, across, maximum, faces, sub_um, secs);
    for (int k = 0; k < fs.size(); ++k) {
        const double w = 2 * M_PI * fs.at(k);
        const std::complex<double> g = cal.gamma.at(k), z = cal.impedance.at(k);
        const std::complex<double> zl = g * z, yl = g / z;
        std::printf("%5.1f GHz eps %.4f alpha %.4f Np/m Z %.3f%+.3fj  R %.2f L %.5g G %.4g C %.5g  residual %.2g\n", fs.at(k) / 1e9,
                    std::pow(g.imag() * c0 / w, 2), g.real(), z.real(), z.imag(), zl.real(), zl.imag() / w, yl.real(), yl.imag() / w,
                    cal.residual.at(k));
    }
    return 0;
}
