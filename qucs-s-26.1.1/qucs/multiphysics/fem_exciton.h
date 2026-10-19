/*
 * fem_exciton.h - a quantum well's excitons with their subband pairs coupled:
 *                 the electron and hole subbands' pairs on one in-plane k
 *                 grid, the Coulomb attraction within and between them (its
 *                 form factors of the states' envelopes), one Hamiltonian's
 *                 bound states and continuum, and each state's optical
 *                 strength - the momentum-space exciton (Chuang, Schmitt-
 *                 Rink, Miller and Chemla 1991; the subbands coupled, Winkler
 *                 1995)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_EXCITON_H
#define QUCS_FEM_EXCITON_H

#include <QString>

#include <complex>
#include <functional>
#include <vector>

namespace qucs_s::fem::exciton {

/// The states' envelopes at points along the growth axis, each scaled by
/// the square root of its point's weight - a density between two states
/// the product, summing to their overlap.
struct Envelopes {
    std::vector<double> z;                                               ///< m, rising
    std::vector<std::vector<double>> electrons;                          ///< [state][point]
    std::vector<std::vector<std::vector<std::complex<double>>>> holes;   ///< [state][component][point]
};

/// A subband pair: its electron's and hole's states (Envelopes'), its
/// transition energy at in-plane k (1/m; J), and its optical amplitudes
/// there - each component's, of the caller's (a polarization and a spin).
struct Pair {
    int electron = 0, hole = 0;
    std::function<double(double)> energy;
    std::function<void(double, std::vector<std::complex<double>>&)> amplitude;
};

struct Options {
    double epsr = 12.9;   ///< static relative permittivity
    double reach = 0;     ///< J: each pair's continuum this far above its edge at least (0.3 eV: 0)
    double spacing = 0;   ///< J: its states this far apart at most there (1 meV: 0)
    int components = 1;   ///< the amplitudes' components
    int maxSize = 1600;   ///< pairs × k points at most: the spacing widened past it, said
    bool coupled = true;  ///< false: each pair alone, its own attraction only
    /// Tests': the form factor of pairs a and b at q (1/m) in place of the
    /// envelopes' - 1 within a pair and 0 between pairs: the ideal 2D
    /// exciton.
    std::function<double(int, int, double)> formFactor;
};

struct Result {
    std::vector<double> energy;                  ///< the states' (J), rising - lines those below 'cut'
    std::vector<std::vector<double>> strength;   ///< [component][state]: |Σ A u|², 1/m² (|ψ(0)|²|A|²)
    /// Each state's cell (J): its largest part's - a pair at a k point -
    /// share of the plane, in its pair's energies, moved by the state's
    /// shift; a continuum's state its pair's density over it.
    std::vector<double> lo, hi;
    double kmax = 0;                             ///< 1/m
    double spacing = 0;                          ///< J: the continuum's spacing used
    int points = 0;                              ///< k points
    /// Each pair's energy at kmax and, past it, each component's density
    /// (1/(J m²)): the free continuum the grid does not hold, its line's
    /// tail the caller's.
    std::vector<double> top;
    std::vector<std::vector<double>> beyond;   ///< [pair][component]
    /// J: the least pair's top - the states above it of the grid's tail to
    /// k infinite (a bound state's short range), coarse: the free continuum
    /// past each pair's top stands for them.
    double cut = 0;
    QString note;                              ///< what was cut to fit (empty: nothing)
};

/// The excitons of \a pairs: their Hamiltonian in k (s states: the light
/// sees no other), its states and their optical strengths.
Result solve(const Envelopes& envelopes, const std::vector<Pair>& pairs, const Options& options);

/// The complete elliptic integral of the first kind, K(m), m = k² < 1.
double ellipticK(double m);

} // namespace qucs_s::fem::exciton

#endif // QUCS_FEM_EXCITON_H
