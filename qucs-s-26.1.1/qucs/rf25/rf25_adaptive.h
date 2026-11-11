/*
 * rf25_adaptive.h - RF 2.5D's adaptive frequency sweep and its mesh
 *                   convergence check: a rational fit of the ports' Z over
 *                   frequency that says where to solve next and fills in
 *                   the rest of the sweep, and how a value read on meshes
 *                   ever finer is settling, extrapolated where it does
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_RF25_ADAPTIVE_H
#define QUCS_RF25_ADAPTIVE_H

#include "rf25_deembed.h"
#include "rf25_fullwave.h"
#include "rf25_static.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <complex>
#include <functional>

namespace qucs_s::rf25 {

/*!
 * A rational function of frequency in barycentric form, every quantity of a
 * set sharing one denominator: set-valued AAA (Nakatsukasa, Sète and
 * Trefethen, SIAM J. Sci. Comput. 40, 2018; Lietaert, Meerbergen, Pérez and
 * Vandereycken, 2022). At frequency f each quantity is
 *
 *     r(f) = sum_j c_j(f) F_j,   c_j(f) = (w_j / (f - f_j)) / sum_k w_k / (f - f_k),
 *
 * F_j its value at the j'th support frequency f_j. The c_j are the same for
 * every quantity, so a quantity not fitted - a port's currents, beside the
 * Z they were fitted on - is carried by the same formula from its values at
 * the support frequencies: they have the structure's poles.
 */
struct Rational {
    QList<int> support;            ///< the samples taken as support points, by index
    QList<double> at;              ///< their frequencies, Hz
    QList<std::complex<double>> weights;
    /// The largest difference, over every sample not a support point and
    /// every quantity, between the fit and the sample, each quantity over its
    /// own largest magnitude (nought where every sample is a support point).
    double residual = 0;
    bool ok() const { return !support.isEmpty(); }
    /// The c_j at \a frequency (Hz): at a support frequency, one for it.
    QList<std::complex<double>> coefficients(double frequency) const;
};

/*!
 * Fits \a values - [sample][quantity], at \a frequencies (Hz) - by
 * set-valued AAA: support points added where the fit is worst, one at a
 * time, until it is within \a tolerance of every sample (each quantity over
 * its largest magnitude) or as many as \a most (0: as many as the samples
 * can determine - the least-squares rows no fewer than the weights).
 */
Rational fitRational(const QList<double>& frequencies, const QList<QList<std::complex<double>>>& values,
                     double tolerance = 1e-7, int most = 0);
/// Each quantity of \a values at \a frequency through \a fit: values as
/// fitted, [sample][quantity].
QList<std::complex<double>> rationalAt(const Rational& fit, const QList<QList<std::complex<double>>>& values,
                                       double frequency);

/// How an adaptive sweep goes about it (the study's "Add points where it
/// changes").
struct AdaptiveSettings {
    /// The most any term of S - each port into its own reference - may
    /// differ between what the fit said of a frequency and what solving it
    /// found, at the frequencies it was least sure of.
    double tolerance = 2e-3;
    int first = 5;                 ///< frequencies solved first, the sweep's ends among them, evenly by index
    int batch = 4;                 ///< the most added at once (solved side by side), at most one between two solved
    /// A sweep of no more than this many frequencies is solved whole: adding
    /// points would save it little.
    int fewest = 12;
};

/// What an adaptive sweep did.
struct Adapted {
    /// At every frequency of the sweep: those solved as solved, the rest
    /// from the fit (FullWave::solvedFrequencies says which).
    FullWave wave;
    QList<double> solved;          ///< Hz, the frequencies solved, in order
    int rounds = 0;                ///< solves asked for, the first among them
    /// The largest difference in S, at the frequencies solved last, between
    /// the fit before they were solved and the solve; and whether it was
    /// within the tolerance for at least two of them in a row.
    double error = 0;
    bool converged = false;
    bool whole = false;            ///< the sweep was solved whole, as too short to gain by it
    int degree = 0;                ///< the final fit's support points
    QStringList lines;             ///< what it did, in words
};

/*!
 * The sweep \a sweep (Hz, ascending) solved where a rational fit of the
 * ports' Z says it is least sure, and filled in elsewhere from the fit.
 * \a solve solves a list of frequencies (side by side, as fullWaveOf does)
 * and gives what fullWaveOf gives; \a stopped, given, is asked between
 * solves. First the sweep's ends and \a how.first frequencies between, then
 * the midpoint of every gap, then where the fit of all that is solved
 * differs most from the fit before it - at most \a how.batch at once, and
 * no two in one gap - until what the fit said of the frequencies solved
 * last is within \a how.tolerance of S at two of them in a row, or every
 * frequency is solved. A port's currents at a frequency not solved are
 * carried by the fit's coefficients from those at its support frequencies.
 */
Adapted adaptiveSweep(const QList<double>& sweep, const std::function<FullWave(const QList<double>&)>& solve,
                      const AdaptiveSettings& how = {}, const std::function<bool()>& stopped = {});

/*!
 * \a calibration, solved at some of \a sweep's frequencies, at every one of
 * them: its discontinuity, the line's propagation constant and impedance
 * each fitted rationally and read there; a strip's residual and length at a
 * frequency not solved the nearest solved one's. As it was where it already
 * has every frequency, or is not ok.
 */
Calibration calibrationAt(const Calibration& calibration, const QList<double>& sweep);

/// FullWave::asymmetry and gain (and where) taken again over every
/// frequency of \a wave, from its z and s.
void checkReciprocityAndPassivity(FullWave& wave);

// ---- the mesh convergence check ---------------------------------------

/// What a value read on meshes ever finer says of itself.
struct Convergence {
    enum class Kind {
        Settled,       ///< it did not move
        Converging,    ///< its changes shrink as the cells do: extrapolated
        Oscillating,   ///< its changes shrink but turn back: bounded by the last
        NotYet,        ///< its changes do not shrink
        TwoMeshes      ///< two meshes alone: a change, nothing of its trend
    };
    Kind kind = Kind::TwoMeshes;
    /// The order the changes shrink at, as the cells: change ~ h^order
    /// (Converging alone).
    double order = 0;
    /// The extrapolated value (Converging), else the finest mesh's.
    std::complex<double> limit;
    /// How far the finest mesh's value is from the limit: by Richardson's
    /// extrapolation (Converging), else at least the last change.
    double error = 0;
    double ratio = 0;              ///< the last change over the one before it
    /// What that ratio would be at most for a value converging as the root
    /// of the cells' size, of these sizes: 0.71 where they halve.
    double slowest = 0;
};

/*!
 * \a values read on meshes whose cells are \a sizes across (largest first;
 * any unit, as long as one): Richardson's extrapolation from the last three,
 * where their changes shrink - the order found from how fast, of the sizes
 * as they are, not taken as halving - by at least the square root of the
 * cells' (a doubling's ratio under 0.71); where they shrink and turn back,
 * bounded by the last change; where they do not shrink, not yet.
 */
Convergence convergenceOf(const QList<double>& sizes, const QList<std::complex<double>>& values);

/// One value the convergence check watches, read on each mesh.
struct Watched {
    QString name;                  ///< "S21 at 40 GHz", "P1's feed: eps_eff"
    QString unit;                  ///< "", "Ohm", "fF"
    bool complex = false;          ///< a complex value (S), said as its magnitude and phase
    QList<std::complex<double>> values;   ///< on each mesh, the coarsest first
    Convergence verdict;
};

/// What the convergence check watches of a full-wave solve at \a frequencies
/// (each the nearest solved): every term of S (S21 alone of S12 and S21),
/// and of each port calibrated by strips of its feed, its line's eps_eff and
/// Z0 and its own discontinuity, at the top frequency.
QList<Watched> watchedOf(const FullWave& wave, const QList<double>& frequencies);
/// Of a quasi-static solve: each net's capacitance and each two nets'
/// mutual one, fF; and where it is a uniform line, its eps_eff and Z0.
QList<Watched> watchedOf(const Extraction& extraction);

/// One mesh of the check.
struct MeshLevel {
    int asked = 0;                 ///< cells across the narrowest conductor asked for
    /// How many times finer than the model's own mesh it is meant to be:
    /// every size of its settings over it (refinedBy()) - what Richardson's
    /// extrapolation takes its cells' size from, as one over it.
    double factor = 1;
    int across = 0;                ///< and put there (fewer where the solver's cells run out)
    double cell = 0;               ///< µm: how wide they are there
    double smallest = 0, largest = 0;   ///< µm: its smallest and largest cells
    int cells = 0, unknowns = 0;
    double seconds = 0;            ///< its mesh and solve
};

/// A mesh convergence check of an RF 2.5D model: the same solve on meshes
/// ever finer, what each watched value did, and what that says.
struct MeshConvergence {
    bool fullWave = true;
    QList<double> frequencies;     ///< a full-wave check's, Hz
    QList<MeshLevel> levels;
    QList<Watched> values;
    QStringList lines;             ///< the report: its verdict, then each value's
    QStringList problems;
    bool ok() const { return problems.isEmpty() && levels.size() >= 2 && !values.isEmpty(); }
};

/// Adds \a watched, read on one more mesh, to \a check's values: by name, a
/// value one mesh missed left out.
void addWatched(MeshConvergence& check, const QList<Watched>& watched);
/// Each value's verdict (convergenceOf() of the levels' cells) and the
/// report's lines.
void judge(MeshConvergence& check);

/// The meshes a check takes where none are asked for, from the mesh's own
/// cells across the narrowest conductor: a quarter, a half and all of them
/// (at least one, two and three), the finest the model's own - what it
/// says is how far the model's mesh is from the limit.
QList<int> defaultLevels(int cellsAcross);

} // namespace qucs_s::rf25

#endif
