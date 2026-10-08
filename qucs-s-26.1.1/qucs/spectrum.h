/*
 * spectrum.h - a transient's spectrum: resampled evenly, windowed, its
 * lines; the fundamental, the harmonics, THD, SFDR, SNR and SINAD
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef QUCS_SPECTRUM_H
#define QUCS_SPECTRUM_H

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <limits>

namespace qucs_s::spectrum {

enum class Window { Rectangular = 0, Hann, Hamming, Blackman, BlackmanHarris, FlatTop };
/// Their names, as the tools and the files say them ("hann").
QStringList windowNames();
QString windowName(Window w);
/// -1 when \a name is none of them.
int windowOf(const QString& name);
/// How many bins either side of a line's peak its main lobe holds.
int lobeBins(Window w);

/// A spectrum: one-sided, amplitude a sine of it would have (the window's
/// coherent gain taken out), and the power in each bin (its noise gain
/// taken out: they sum to the signal's mean square).
struct Spectrum {
    double df = 0;               ///< Hz a bin
    QVector<double> amplitude;   ///< bin 0 the DC
    QVector<double> power;
    int points = 0;              ///< resampled to
    Window window = Window::Hann;
};

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

/// The spectrum of y over x (a transient, x rising), from \a from on (NaN:
/// all of it): resampled evenly at a power of two of points (at least as
/// many as it has, at most 65536), windowed.
Spectrum of(const QVector<double>& x, const QVector<double>& y, Window window, double from = NaN);

struct Line {
    int harmonic = 0;   ///< 1 the fundamental
    double frequency = 0, amplitude = 0, dBc = 0;
};

/// What the spectrum says of a periodic signal.
struct Analysis {
    bool ok = false;
    QString error;
    double fundamental = 0, amplitude = 0;
    QVector<Line> harmonics;      ///< 1 (the fundamental) up to the highest asked that is below Nyquist
    double thd = 0;               ///< the harmonics' rms over the fundamental's, a ratio
    double sfdr = 0;              ///< dB from the fundamental to the strongest other line (spur)
    double spurFrequency = 0;
    double snr = 0;               ///< dB: the fundamental's power over the noise's (not the harmonics, not DC)
    double sinad = 0;             ///< dB: over noise and harmonics
    double noiseFloorDbc = 0;     ///< the median bin, dBc
};

/// \a s analysed: the fundamental (\a fundamental, NaN: the strongest
/// line), its harmonics up to \a harmonics, each line's power its main
/// lobe's.
Analysis analyse(const Spectrum& s, double fundamental = NaN, int harmonics = 9);

/// As the tools say it.
QJsonObject toJson(const Analysis& a, const Spectrum& s);

} // namespace qucs_s::spectrum

#endif
