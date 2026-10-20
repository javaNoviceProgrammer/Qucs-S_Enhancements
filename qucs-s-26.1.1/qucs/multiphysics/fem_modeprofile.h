/*
 * fem_modeprofile.h - a mode analysis's mode on a regular grid: its six
 *                     field components (complex), sampled from a solution,
 *                     between the grid's points, as files (MATLAB's .mat,
 *                     NumPy's .npz, CSV) and read back from a .mat; and a
 *                     mode's overlap with another field
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_MODEPROFILE_H
#define QUCS_FEM_MODEPROFILE_H

#include <QByteArray>
#include <QPointF>
#include <QString>

#include <array>
#include <complex>
#include <limits>
#include <optional>
#include <vector>

namespace qucs_s::fem {

class Solution;

/// A mode's fields on a regular grid: Ex, Ey, Ez (V/m), Hx, Hy, Hz (A/m)
/// at each point, x the fastest (index iy nx + ix), as found (a phase of
/// 0 at z = 0: E e^(j(ωt - βz))); x and y in metres, rising.
struct ModeProfile {
    std::vector<double> x, y;
    std::array<std::vector<std::complex<double>>, 6> F;
    std::complex<double> neff = 1;   ///< β/k0 as neff - j neff_im
    double lambda0 = std::numeric_limits<double>::quiet_NaN();   ///< m, in vacuum
    int mode = 0;                    ///< its number (from 1)
    QString label;                   ///< what it is of ("model.qfem: emw, mode 1, λ = 1.55 µm")

    int nx() const { return int(x.size()); }
    int ny() const { return int(y.size()); }
    /// Its fields at \a p (m): between the four grid points round it
    /// (bilinear), nought outside the grid. False outside.
    bool at(QPointF p, std::complex<double>* E, std::complex<double>* H) const;
    /// ∫ (E × H*)·z over the grid (the trapezoid rule; W: twice its power
    /// along z, ½ Re of it).
    std::complex<double> flux() const;
};

/// Mode analysis physics \a field's mode, the one the solution has
/// selected, at the points of a grid \a x by \a y (m): nought where it is
/// not defined (outside its domains, in a perfectly matched layer).
ModeProfile sampleMode(const Solution& solution, int field, const std::vector<int>& pmlDomains, const std::vector<double>& x,
                       const std::vector<double>& y);

/// Modes as a MATLAB 5 .mat file (uncompressed: MATLAB, Octave, SciPy's
/// loadmat, matlabload in Lumerical): x, y (m, columns), Ex... Hz complex of
/// dimensions [nx ny] (x the first index) - [nx ny M] for M modes -, neff
/// (complex, β/k0), lambda0 (m), mode, power (W, ½ Re ∫ E × H*·z over the
/// grid) each 1 by M, and a text about them.
QByteArray profilesMat(const std::vector<ModeProfile>& modes, const QString& about);
/// As NumPy's .npz (a zip of .npy, stored): x, y (m), Ex... Hz complex of
/// shape (ny, nx) - (M, ny, nx) for M modes -, neff (complex), lambda0,
/// mode, power, about.
QByteArray profilesNpz(const std::vector<ModeProfile>& modes, const QString& about);
/// As CSV: # lines of each mode's numbers, then a row per point - mode, x,
/// y (m), the real and imaginary part of each component.
QString profilesCsv(const std::vector<ModeProfile>& modes, const QString& about);

/// A mode read back from a .mat (MATLAB 5, uncompressed - as profilesMat()
/// writes, SciPy's savemat, MATLAB's save -v6): x, y (m), Ex and Ey at the
/// least, Ez, Hx, Hy, Hz if there (H missing: a plane wave's, (neff/η0) z ×
/// E), neff if there (else 1); of several, the \a index th (from 1) - its
/// number the file's mode if there. Each [nx ny] (x the first index) or,
/// the grid not square, [ny nx] - turned round.
std::optional<ModeProfile> readProfileMat(const QByteArray& data, int index, QString* error);

} // namespace qucs_s::fem

#endif // QUCS_FEM_MODEPROFILE_H
