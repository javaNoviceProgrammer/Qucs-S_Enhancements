/*
 * eyeanalysis.h - the eye of a serial data signal: a transient folded at
 * its unit interval, and what is measured on it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_EYEANALYSIS_H
#define QUCS_EYEANALYSIS_H

#include "dataset.h"

#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <limits>

/*!
 * The eye of a data signal (NRZ, or PAM4's four levels), as ngspice's eye
 * command and a sampling oscilloscope measure it.
 *
 * The signal crosses a decision threshold between neighbouring levels at
 * the bit boundaries. Folded at the unit interval (UI), the crossings
 * gather at one phase; the eye's centre, the sampling instant, is half a
 * UI from them. There each symbol's value is on one of the levels; the
 * eye's height is the lowest value on the upper level less the highest on
 * the lower one, its width a UI less the spread of the crossings (the
 * peak-to-peak jitter).
 */
namespace qucs_s::eye {

inline constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

struct Options {
    double ui = NaN;          ///< the unit interval; NaN: told from the crossings
    double start = NaN;       ///< the eye from here on (the settling before it left out); NaN: from the curve's start
    int levels = 2;           ///< 2: NRZ, 4: PAM4
    double threshold = NaN;   ///< NRZ: the decision threshold; NaN: halfway between the levels
    double maskWidth = NaN;   ///< the mask: its width in UI (0 to 1) ...
    double maskHeight = NaN;  ///< ... and its height (in the signal's unit); NaN: no mask
};

/// One eye, between two neighbouring levels: NRZ has one, PAM4 three
/// (lowest first).
struct Eye {
    double threshold = NaN;          ///< its decision threshold
    double low = NaN, high = NaN;    ///< the levels below and above: the means of the symbols on them at the centre
    double inner = NaN, outer = NaN; ///< the highest symbol of the lower level and the lowest of the upper, at the centre
    double height = NaN;             ///< inner opening at the centre: lowest upper less highest lower (below 0: closed)
    double width = NaN;              ///< a UI less the crossings' spread (0: closed)
    double widthBer12 = NaN;         ///< a UI less 14.069 rms jitter: the width at a bit error rate of 1e-12, the jitter Gaussian
    double jitterRms = NaN;          ///< of the crossings about their mean
    double jitterPp = NaN;           ///< the crossings' spread
    double phase = NaN;              ///< the crossings' mean phase, in UI from the centre (about -0.5)
    double earliest = NaN;           ///< the earliest and latest crossings, in UI from their mean
    double latest = NaN;
    double q = NaN;                  ///< (high - low) / (sigma high + sigma low); NaN without noise
    int crossings = 0;
    int lower = 0, upper = 0;        ///< the symbols on either level at the centre
};

struct Result {
    QString error;               ///< why there is no eye (empty: there is one)
    double ui = NaN;
    bool uiEstimated = false;    ///< told from the crossings, not given
    double start = NaN, end = NaN;
    double centre = NaN;         ///< the eye's centre: the time of a sampling instant (others a UI apart)
    QVector<double> levels;      ///< lowest first
    QList<Eye> eyes;             ///< lowest first
    int symbols = 0;             ///< the sampling instants in [start, end]
    int maskHits = -1;           ///< the UIs whose trace enters the mask (-1: no mask)
    QStringList notes;
    bool ok() const { return error.isEmpty(); }
};

/// The eye of \a c (x: time, y: the signal).
Result analyse(const dataset::Curve& c, const Options& options);

/// Where \a c crosses \a level, leaving a band of \a hysteresis either side
/// of it: ringing within the band is no crossing. The time of each is
/// where it last crossed the level itself.
QVector<double> crossingTimes(const dataset::Curve& c, double level, double hysteresis);

/// The unit interval of crossings that fall at bit boundaries: the one all
/// intervals between them are whole multiples of. NaN when it cannot be
/// told (\a why says why).
double estimateUi(const QVector<double>& crossings, QString* why = nullptr);

/// \a ui made more exact: \a crossings' times fitted (least squares)
/// against the number of the UI each falls in.
double fitted(const QVector<double>& crossings, double ui);

/// The mask: a hexagon \a width UI wide and \a height high, centred on
/// (0, 0) - in UI from the eye's centre, and from the threshold.
QPolygonF mask(double width, double height);

/// The curve from \a start cut into windows \a span UI long, starting a UI
/// apart from \a origin (so a sample is in \a span of them, as a sampling
/// oscilloscope triggered every UI shows it): \a each gets every window as
/// a polyline of (time from the window's start, value), its ends where the
/// curve crosses them. Returns the number of windows.
int fold(const dataset::Curve& c, double start, double ui, double origin, int span,
         const std::function<void(const QVector<QPointF>&)>& each);

/// \a r as JSON, its numbers to 7 places: the unit interval, the levels,
/// each eye (its height, width, jitter, Q, threshold), the symbols, the
/// mask's hits, the notes; or {"error"}.
QJsonObject toJson(const Result& r);

/// Where a window of \a span UI starts (\a origin for fold()) for the eye's
/// centre to be in its middle.
double originFor(const Result& r, int span);

} // namespace qucs_s::eye

#endif // QUCS_EYEANALYSIS_H
