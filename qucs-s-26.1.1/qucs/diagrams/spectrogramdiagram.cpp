/*
 * spectrogramdiagram.cpp - the spectrogram: a transient's spectrum as it
 * goes (see spectrogramdiagram.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "spectrogramdiagram.h"

#include <algorithm>
#include <cmath>

namespace sp = qucs_s::spectrum;

namespace {

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kMostRows = 512;
constexpr int kMostColumns = 1024;

QString fieldText(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 15) : QStringLiteral("-");
}

double fieldValue(const QString& s)
{
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok && std::isfinite(v) ? v : NaN;
}

// y at x, straight between the samples (x rising).
double at(const QVector<double>& x, const QVector<double>& y, double t)
{
    const int i = int(std::upper_bound(x.cbegin(), x.cend(), t) - x.cbegin());
    if (i <= 0) return y.first();
    if (i >= x.size()) return y.last();
    const double f = x.at(i) != x.at(i - 1) ? (t - x.at(i - 1)) / (x.at(i) - x.at(i - 1)) : 0.0;
    return y.at(i - 1) + f * (y.at(i) - y.at(i - 1));
}

} // namespace

SpectrogramDiagram::SpectrogramDiagram(int cx, int cy) : ContourDiagram(cx, cy), segment(NaN), upTo(NaN)
{
    Name = "Spectrogram";
    levels = 0;        // (no iso-lines unless asked)
    labels = false;
    map = Turbo;
    calcDiagram();
}

Diagram* SpectrogramDiagram::newOne()
{
    return new SpectrogramDiagram();
}

Element* SpectrogramDiagram::info(QString& Name, char*& BitmapFile, bool getNewOne)
{
    Name = QObject::tr("Spectrogram");
    BitmapFile = (char*)"spectrogram";
    if (getNewOne) return new SpectrogramDiagram();
    return nullptr;
}

double SpectrogramDiagram::segmentOf(const Graph* g) const
{
    const DataX* xs = g->axis(0);
    if (xs == nullptr || xs->Points == nullptr || xs->count < 2) return NaN;
    const double span = xs->Points[xs->count - 1] - xs->Points[0];
    if (!(span > 0)) return NaN;
    return std::isfinite(segment) && segment > 0 && segment <= span ? segment : span / 16.0;
}

ContourDiagram::Grid SpectrogramDiagram::gridFor(const Graph* g) const
{
    Grid grid;
    const DataX* xs = g->axis(0);
    // (Over frequency - an AC sweep - there is no time to cut: bug hunt of
    // 2026-10-08, B7.)
    if (xs != nullptr && xs->Var.contains(QLatin1String("freq"), Qt::CaseInsensitive)) {
        grid.error = QObject::tr("%1 is over %2 (an AC run's): a spectrogram cuts a transient, over time").arg(g->Var.section(QLatin1Char('/'), -1), xs->Var);
        return grid;
    }
    if (g->cPointsY == nullptr || xs == nullptr || xs->Points == nullptr || xs->count < 16) {
        grid.error = QObject::tr("no transient to cut: simulate, or check the variable's name (at least 16 samples)");
        return grid;
    }
    // The samples, time rising (a complex value's magnitude).
    QVector<double> t, y;
    for (int i = 0; i < xs->count; ++i) {
        const double re = g->cPointsY[2 * i], im = g->cPointsY[2 * i + 1];
        const double v = std::fabs(im) > 1e-250 ? std::hypot(re, im) : re;
        if (!std::isfinite(xs->Points[i]) || !std::isfinite(v) || (!t.isEmpty() && xs->Points[i] <= t.last())) continue;
        t << xs->Points[i];
        y << v;
    }
    const double length = segmentOf(g);
    if (t.size() < 16 || !(length > 0)) {
        grid.error = QObject::tr("too short a transient to cut into segments");
        return grid;
    }
    // A segment holds 8 samples or more, as a spectrum needs: a shorter one
    // gives none, and cut a run into billions of steps (bug hunt of
    // 2026-10-08, F2: a file that kept one could not be opened).
    const double interval = (t.last() - t.first()) / (t.size() - 1);
    if (length < 8 * interval) {
        grid.error = QObject::tr("a segment of %1 s holds fewer than 8 of the run's samples (%2 s apart on average): give a "
                                 "longer segment, %3 s or more")
                         .arg(length)
                         .arg(interval)
                         .arg(8 * interval);
        return grid;
    }
    // At most kMostColumns steps over the whole run, each segment's start
    // counted whether it gives a spectrum or not.
    const double hop = std::max({length * (1.0 - std::clamp(overlap, 0.0, 0.9)), length / 10.0,
                                 (t.last() - t.first() - length) / (kMostColumns - 1)});
    // Each segment: its samples, its ends straight between them - every one
    // as long, its bins as wide (1 / its length).
    QList<sp::Spectrum> spectra;
    int steps = 0;
    for (double s = t.first(); s + length <= t.last() + 1e-12 * length && steps < kMostColumns; s += hop, ++steps) {
        QVector<double> st, sy;
        st << s;
        sy << at(t, y, s);
        for (int i = int(std::upper_bound(t.cbegin(), t.cend(), s) - t.cbegin()); i < t.size() && t.at(i) < s + length; ++i) {
            st << t.at(i);
            sy << y.at(i);
        }
        st << s + length;
        sy << at(t, y, s + length);
        const sp::Spectrum spectrum = sp::of(st, sy, window);
        if (spectrum.amplitude.isEmpty()) continue;
        spectra << spectrum;
        grid.x << s + length / 2;
    }
    if (spectra.size() < 2) {
        grid.error = QObject::tr("fewer than two segments of %1 s with a spectrum: give a shorter segment (a run of %2 s), or "
                                 "a longer one where the samples are sparse")
                         .arg(length)
                         .arg(t.last() - t.first());
        return grid;
    }
    // Up to the highest frequency every segment has (at most 512 rows) -
    // or the one given, or a quarter past the highest within range of the
    // loudest (what is there to see).
    int bins = kMostRows;
    for (const sp::Spectrum& s : std::as_const(spectra)) bins = std::min(bins, int(s.amplitude.size()));
    const double df = spectra.first().df;
    if (std::isfinite(upTo) && upTo > 0) {
        bins = std::clamp(int(std::ceil(upTo / df)) + 1, 2, bins);
    } else {
        double loudest = -400;
        for (const sp::Spectrum& s : std::as_const(spectra))
            for (int j = 1; j < bins; ++j) loudest = std::max(loudest, 20.0 * std::log10(std::max(s.amplitude.at(j), 1e-20)));
        int highest = 1;
        for (const sp::Spectrum& s : std::as_const(spectra))
            for (int j = bins - 1; j > highest; --j)
                if (20.0 * std::log10(std::max(s.amplitude.at(j), 1e-20)) >= loudest - range) {
                    highest = j;
                    break;
                }
        bins = std::clamp(int(std::ceil(1.25 * highest)) + 2, 2, bins);
    }
    for (int j = 0; j < bins; ++j) grid.y << j * df;
    grid.xName = xs->Var;
    grid.yName = QStringLiteral("frequency");
    const int nx = int(grid.x.size());
    grid.v.resize(bins * nx);
    for (int i = 0; i < nx; ++i)
        for (int j = 0; j < bins; ++j) {
            const double a = spectra.at(i).amplitude.at(j);
            grid.v[j * nx + i] = a > 0 ? 20.0 * std::log10(a) : -400.0;
        }
    return grid;
}

void SpectrogramDiagram::autoRange(double, double hi)
{
    // The loudest at the top (a whole dB), range dB below it at the bottom.
    zAxis.up = std::ceil(hi);
    zAxis.low = zAxis.up - (range > 0 ? range : 80.0);
}

QString SpectrogramDiagram::colourName() const
{
    return Graphs.isEmpty() ? QString() : Graphs.first()->Var.section(QLatin1Char('/'), -1) + QStringLiteral(", dB");
}

void SpectrogramDiagram::createAxisLabels()
{
    const QString x = xAxis.Label, y = yAxis.Label;
    if (y.isEmpty()) yAxis.Label = QObject::tr("frequency (Hz)");
    ContourDiagram::createAxisLabels();
    xAxis.Label = x;
    yAxis.Label = y;
}

QString SpectrogramDiagram::extraSaveFields() const
{
    return ContourDiagram::extraSaveFields()
           + QStringLiteral(" %1 %2 %3 %4 %5").arg(int(window)).arg(fieldText(segment), fieldText(overlap), fieldText(range), fieldText(upTo));
}

void SpectrogramDiagram::loadExtraFields(const QStringList& fields)
{
    ContourDiagram::loadExtraFields(fields.mid(0, 6));
    bool ok = false;
    const int w = fields.value(6).toInt(&ok);
    window = ok && w >= 0 && w <= int(sp::Window::FlatTop) ? sp::Window(w) : sp::Window::Hann;
    segment = fieldValue(fields.value(7));
    if (!(segment > 0)) segment = NaN;
    const double o = fieldValue(fields.value(8));
    overlap = o >= 0 && o <= 0.9 ? o : 0.5;
    const double r = fieldValue(fields.value(9));
    range = r > 0 && r <= 400 ? r : 80;
    upTo = fieldValue(fields.value(10));
    if (!(upTo > 0)) upTo = NaN;
}
