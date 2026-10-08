/*
 * spectrum.cpp - a transient's spectrum: resampled evenly, windowed, its
 * lines; the fundamental, the harmonics, THD, SFDR, SNR and SINAD
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "spectrum.h"

#include <QJsonArray>
#include <QObject>

#include <algorithm>
#include <cmath>
#include <complex>

namespace qucs_s::spectrum {

namespace {

constexpr double Pi = 3.14159265358979323846;

double coefficient(Window w, int i, int n)
{
    const double t = 2 * Pi * i / n;   // (periodic: the spectrum's)
    switch (w) {
    case Window::Rectangular: return 1.0;
    case Window::Hann: return 0.5 - 0.5 * std::cos(t);
    case Window::Hamming: return 0.54 - 0.46 * std::cos(t);
    case Window::Blackman: return 0.42 - 0.5 * std::cos(t) + 0.08 * std::cos(2 * t);
    case Window::BlackmanHarris: return 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
    case Window::FlatTop:
        return 0.21557895 - 0.41663158 * std::cos(t) + 0.277263158 * std::cos(2 * t) - 0.083578947 * std::cos(3 * t)
               + 0.006947368 * std::cos(4 * t);
    }
    return 1.0;
}

void fft(QVector<std::complex<double>>& a)
{
    const int n = int(a.size());
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const std::complex<double> w1 = std::polar(1.0, -2 * Pi / len);
        for (int i = 0; i < n; i += len) {
            std::complex<double> wk(1, 0);
            for (int k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k], t = a[i + k + len / 2] * wk;
                a[i + k] = u + t;
                a[i + k + len / 2] = u - t;
                wk *= w1;
            }
        }
    }
}

double dB(double ratio)
{
    return ratio > 0 ? 20.0 * std::log10(ratio) : -400.0;
}

} // namespace

QStringList windowNames()
{
    return {QStringLiteral("rectangular"), QStringLiteral("hann"), QStringLiteral("hamming"), QStringLiteral("blackman"),
            QStringLiteral("blackman_harris"), QStringLiteral("flat_top")};
}

QString windowName(Window w)
{
    return windowNames().value(int(w));
}

int windowOf(const QString& name)
{
    return int(windowNames().indexOf(name.trimmed().toLower().replace(QLatin1Char(' '), QLatin1Char('_'))));
}

int lobeBins(Window w)
{
    switch (w) {
    case Window::Rectangular: return 1;
    case Window::Hann: case Window::Hamming: return 2;
    case Window::Blackman: return 3;
    case Window::BlackmanHarris: return 4;
    case Window::FlatTop: return 5;
    }
    return 2;
}

Spectrum of(const QVector<double>& x, const QVector<double>& y, Window window, double from)
{
    Spectrum s;
    s.window = window;
    // The samples in range, x rising.
    QVector<double> xs, ys;
    for (int i = 0; i < x.size() && i < y.size(); ++i) {
        if (!std::isfinite(x.at(i)) || !std::isfinite(y.at(i))) continue;
        if (std::isfinite(from) && x.at(i) < from) continue;
        if (!xs.isEmpty() && x.at(i) <= xs.last()) continue;
        xs << x.at(i);
        ys << y.at(i);
    }
    if (xs.size() < 8) return s;
    const double span = xs.last() - xs.first();
    int n = 64;
    while (n < xs.size() && n < 65536) n *= 2;
    const double dt = span / n;
    QVector<std::complex<double>> a(n);
    double sum = 0, squares = 0;
    int j = 0;
    for (int i = 0; i < n; ++i) {
        const double t = xs.first() + i * dt;
        while (j + 1 < xs.size() - 1 && xs.at(j + 1) < t) ++j;
        const double u = xs.at(j + 1) == xs.at(j) ? 0.0 : (t - xs.at(j)) / (xs.at(j + 1) - xs.at(j));
        const double v = ys.at(j) + std::clamp(u, 0.0, 1.0) * (ys.at(j + 1) - ys.at(j));
        const double w = coefficient(window, i, n);
        sum += w;
        squares += w * w;
        a[i] = v * w;
    }
    fft(a);
    const double coherent = sum / n, noise = squares / n;
    s.df = 1.0 / (n * dt);
    s.points = n;
    s.amplitude.resize(n / 2);
    s.power.resize(n / 2);
    for (int k = 0; k < n / 2; ++k) {
        const double m = std::abs(a[k]);
        s.amplitude[k] = m / (n * coherent) * (k == 0 ? 1 : 2);
        s.power[k] = m * m / (double(n) * n * noise) * (k == 0 ? 1 : 2);
    }
    return s;
}

Analysis analyse(const Spectrum& s, double fundamental, int harmonics)
{
    Analysis r;
    const int bins = int(s.amplitude.size());
    const int lobe = lobeBins(s.window);
    if (bins < 4 * lobe + 4) {
        r.error = QObject::tr("too few samples for a spectrum");
        return r;
    }
    // The strongest bin within +/- lobe of k; the power of its lobe.
    const auto peakNear = [&](int k) {
        int best = std::clamp(k, 0, bins - 1);
        for (int i = std::max(0, k - lobe); i <= std::min(bins - 1, k + lobe); ++i)
            if (s.amplitude.at(i) > s.amplitude.at(best)) best = i;
        return best;
    };
    QVector<bool> taken(bins, false);
    const auto lobePower = [&](int k, bool take) {
        double p = 0;
        for (int i = std::max(0, k - lobe); i <= std::min(bins - 1, k + lobe); ++i) {
            if (taken.at(i)) continue;
            p += s.power.at(i);
            if (take) taken[i] = true;
        }
        return p;
    };
    lobePower(0, true);   // DC and its lobe: neither signal nor noise
    // The fundamental: given, else the strongest line.
    int k1 = -1;
    if (std::isfinite(fundamental) && fundamental > 0) {
        k1 = peakNear(int(std::lround(fundamental / s.df)));
    } else {
        for (int i = lobe + 1; i < bins; ++i)
            if (k1 < 0 || s.amplitude.at(i) > s.amplitude.at(k1)) k1 = i;
    }
    if (k1 <= lobe || k1 >= bins - 1) {
        r.error = QObject::tr("no fundamental line in the spectrum (below the window's lobe of DC, or at its end)");
        return r;
    }
    // A peak's place between the bins: a parabola through the logarithms
    // of its bin and the two beside it (a sine of 10.5 periods was read
    // half a bin off, and its harmonics sought at multiples of that: bug
    // hunt of 2026-10-08, A3).
    const auto between = [&](int k) {
        if (k <= 0 || k >= bins - 1) return double(k);
        const double a = std::log(std::max(s.amplitude.at(k - 1), 1e-300)), b = std::log(std::max(s.amplitude.at(k), 1e-300)),
                     c = std::log(std::max(s.amplitude.at(k + 1), 1e-300));
        const double d = a - 2 * b + c;
        return d < 0 ? k + std::clamp(0.5 * (a - c) / d, -0.5, 0.5) : double(k);
    };
    const double p1 = lobePower(k1, true);
    if (!(p1 > 0)) {
        r.error = QObject::tr("the fundamental has no power");
        return r;
    }
    const double f1 = between(k1);
    r.fundamental = f1 * s.df;
    r.amplitude = std::sqrt(2 * p1);
    r.harmonics << Line{1, r.fundamental, r.amplitude, 0.0};
    // The floor: the median bin past DC's lobe. A harmonic is a line above
    // it (10 dB or more); a ripple of the floor is none.
    QVector<double> floorBins = s.amplitude.mid(lobe + 1);
    std::sort(floorBins.begin(), floorBins.end());
    const double floor = floorBins.isEmpty() ? 0.0 : floorBins.at(floorBins.size() / 2);
    double harmonicPower = 0;
    for (int h = 2; h <= harmonics; ++h) {
        // Where it is: h times the fundamental. A line there is a peak - a
        // bin above both beside it - within a bin of it; else none is, and
        // the bin there tells how little (the strongest bin within the
        // lobe was numbered the harmonic, leakage of another: a square's
        // "2nd" at 1800 Hz).
        const double centre = h * f1;
        const int k = int(std::lround(centre));
        if (k >= bins - lobe) break;
        // (Its place between the bins within 0.75 of a bin of it: a line is
        // nearer than that, a ripple of the floor a bin away is not.)
        int at = -1;
        for (int i = std::max(1, k - 1); i <= std::min(bins - 2, k + 1); ++i)
            if (s.amplitude.at(i) > s.amplitude.at(i - 1) && s.amplitude.at(i) >= s.amplitude.at(i + 1)
                && std::abs(between(i) - centre) <= 0.75 && s.amplitude.at(i) > 3.16 * floor
                && (at < 0 || s.amplitude.at(i) > s.amplitude.at(at)))
                at = i;
        double p, frequency;
        if (at >= 0) {
            p = lobePower(at, true);
            frequency = between(at) * s.df;
        } else {
            p = taken.at(k) ? 0.0 : s.power.at(k);
            taken[k] = true;
            frequency = centre * s.df;
        }
        harmonicPower += p;
        const double amplitude = std::sqrt(2 * p);
        r.harmonics << Line{h, frequency, amplitude, dB(amplitude / r.amplitude)};
    }
    // The noise: every bin not DC's, the fundamental's or a harmonic's.
    double noisePower = 0;
    for (int i = 0; i < bins; ++i)
        if (!taken.at(i)) noisePower += s.power.at(i);
    // The strongest spur: a line outside DC's and the fundamental's lobes
    // (a harmonic counts).
    QVector<bool> spurFree(bins, false);
    for (int i = 0; i <= lobe && i < bins; ++i) spurFree[i] = true;
    for (int i = std::max(0, k1 - lobe); i <= std::min(bins - 1, k1 + lobe); ++i) spurFree[i] = true;
    int spur = -1;
    for (int i = 0; i < bins; ++i)
        if (!spurFree.at(i) && (spur < 0 || s.amplitude.at(i) > s.amplitude.at(spur))) spur = i;
    if (spur >= 0) {
        double p = 0;
        for (int i = std::max(0, spur - lobe); i <= std::min(bins - 1, spur + lobe); ++i)
            if (!spurFree.at(i)) p += s.power.at(i);
        r.sfdr = dB(r.amplitude / std::sqrt(2 * p));
        r.spurFrequency = spur * s.df;
    }
    r.thd = std::sqrt(harmonicPower / p1);
    r.snr = noisePower > 0 ? 10.0 * std::log10(p1 / noisePower) : 400.0;
    r.sinad = noisePower + harmonicPower > 0 ? 10.0 * std::log10(p1 / (noisePower + harmonicPower)) : 400.0;
    QVector<double> sorted = s.amplitude.mid(lobe + 1);
    std::sort(sorted.begin(), sorted.end());
    r.noiseFloorDbc = sorted.isEmpty() ? -400.0 : dB(sorted.at(sorted.size() / 2) / r.amplitude);
    r.ok = true;
    return r;
}

QJsonObject toJson(const Analysis& a, const Spectrum& s)
{
    if (!a.ok) return {{QStringLiteral("error"), a.error}};
    // Seven significant digits, as the other measurements give them.
    const auto round = [](double v) {
        if (!std::isfinite(v) || v == 0.0) return std::isfinite(v) ? v : 0.0;
        const double scale = std::pow(10.0, 6 - std::floor(std::log10(std::fabs(v))));
        return std::round(v * scale) / scale;
    };
    QJsonArray lines;
    for (const Line& l : a.harmonics)
        lines << QJsonObject{{QStringLiteral("harmonic"), l.harmonic},
                             {QStringLiteral("frequency"), round(l.frequency)},
                             {QStringLiteral("amplitude"), round(l.amplitude)},
                             {QStringLiteral("dBc"), round(l.dBc)}};
    return {{QStringLiteral("window"), windowName(s.window)},
            {QStringLiteral("fundamental"), round(a.fundamental)},
            {QStringLiteral("amplitude"), round(a.amplitude)},
            {QStringLiteral("harmonics"), lines},
            {QStringLiteral("thd %"), round(100 * a.thd)},
            {QStringLiteral("thd dB"), round(a.thd > 0 ? 20 * std::log10(a.thd) : -400)},
            {QStringLiteral("sfdr dB"), round(a.sfdr)},
            {QStringLiteral("spur frequency"), round(a.spurFrequency)},
            {QStringLiteral("snr dB"), round(a.snr)},
            {QStringLiteral("sinad dB"), round(a.sinad)},
            {QStringLiteral("noise floor dBc"), round(a.noiseFloorDbc)},
            {QStringLiteral("resolution"), round(s.df)},
            {QStringLiteral("points"), s.points}};
}

} // namespace qucs_s::spectrum
