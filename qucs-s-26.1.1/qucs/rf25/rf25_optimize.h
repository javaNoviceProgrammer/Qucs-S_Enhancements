/*
 * rf25_optimize.h - what an RF 2.5D model's Parametric Sweep and
 *                   Optimization read of a solution: its values by name,
 *                   frequency by frequency (S21_dB, Z11_re, GD21, Z0, C11,
 *                   Lloop...), an expression of them and of the parameters
 *                   over a window of frequencies, judged by its worst, its
 *                   last, its least or most, or its value at one frequency
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_RF25_OPTIMIZE_H
#define QUCS_RF25_OPTIMIZE_H

#include "../multiphysics/fem_expr.h"
#include "../multiphysics/fem_model.h"
#include "../multiphysics/optimizer_core.h"
#include "rf25_solve.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include <limits>

namespace qucs_s::rf25 {

/*!
 * A solution's values by name, at each of its frequencies:
 *
 * - a full wave's, for each pair of ports i, j (from 1): S21_dB, S21_mag,
 *   S21_deg, S21_re, S21_im; Z21_re, Z21_im, Z21_mag (Ohm); GD21, the group
 *   delay -d(phase)/d(omega) (s, where there are two frequencies at least);
 *   VSWR1 for each port; written S2_1 too, as they must be past nine ports;
 * - a quasi-static solve's, at each of the study's frequencies: C11, the
 *   capacitance matrix (F, the nets' order); of a uniform line, Z0 (Ohm),
 *   eps_eff, R, L, G and C per metre (Ohm/m, H/m, S/m, F/m) and alpha_dB
 *   (dB/m) of its first net, Z0_2... of the others; of a loop two ports make,
 *   Lloop (H) and Rloop (its resistance at direct current, Ohm);
 * - f, the frequency (Hz).
 *
 * A name a solution has not is not there: an expression of it is refused
 * with the names it has.
 */
struct SolutionValues {
    QList<double> frequencies;          ///< Hz, in order
    QStringList names;
    QList<fem::Dim> dims;               ///< by name
    QList<QVector<double>> at;          ///< by frequency, by name
    int index(const QString& name) const { return int(names.indexOf(name)); }
};
/// \a solution's values; \a model gives a quasi-static solve its frequencies
/// (its study's sweep, else its top).
SolutionValues solutionValues(const ModelSolution& solution, const Model& model);

/// What a sweep's quantity or an objective reads: \a expression, of the
/// values and the parameters, over \a from to \a to (Hz; NaN: the sweep's
/// end), judged as \a over says.
struct Reading {
    enum class Over {
        Worst,   ///< an objective's: by its goal, as an optimization's goals judge a sweep (optimization::measure)
        Last,    ///< the last frequency's
        At,      ///< at \a at, read between the two frequencies about it
        Least,   ///< the least
        Most     ///< the most
    };
    QString expression;
    double from = std::numeric_limits<double>::quiet_NaN();
    double to = std::numeric_limits<double>::quiet_NaN();
    Over over = Over::Worst;
    double at = std::numeric_limits<double>::quiet_NaN();
};
/// "worst", "last", "at", "min", "max" (empty: \a fallback); false where it
/// is none of them.
bool overOf(const QString& text, Reading::Over fallback, Reading::Over* over);
/// \a text as a frequency in \a parameters ("2.4[GHz]", an expression); NaN
/// where it is empty. False and why where it is no frequency.
bool frequencyOf(const QString& text, const fem::ParameterScope& parameters, double* hertz, QString* why);

/// \a expression's value at each of \a values' frequencies within [\a from,
/// \a to], with its unit. False and why: no expression of them, no frequency
/// in the window.
bool seriesOf(const SolutionValues& values, const fem::ParameterScope& parameters, const QString& expression, double from, double to,
              QList<double>* frequencies, QList<double>* series, fem::Dim* dim, QString* why);
/// \a reading of \a values as it judges it; \a goal says what Worst is
/// (a Minimize's worst is its largest, a target's the furthest from it...).
/// False and why where it cannot be.
bool readingValue(const SolutionValues& values, const fem::ParameterScope& parameters, const Reading& reading,
                  const optimization::Goal& goal, double* value, fem::Dim* dim, QString* why);

} // namespace qucs_s::rf25

#endif
