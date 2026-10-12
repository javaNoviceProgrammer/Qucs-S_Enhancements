/*
 * fem_optics.h - light absorbed across a semiconductor's gap: a direct
 *                gap's edge (√(ħω - Eg)/ħω) and its change with a field,
 *                the Franz-Keldysh effect (Airy's functions) - what a mode
 *                analysis's electroabsorption reads, and a photodiode's
 *                generation through it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_OPTICS_H
#define QUCS_FEM_OPTICS_H

namespace qucs_s::fem {

/// Airy's Ai(x) and Ai'(x): a power series near nought, Poincaré's
/// asymptotic expansions beyond (about 1e-7 relative where they meet).
void airy(double x, double* ai, double* aiPrime);

/// A direct gap's absorption coefficient (1/m) at photon energy \a photon
/// (J) - the gap \a gap (J), under a field \a field (V/m), the electron and
/// hole's reduced mass \a mr (m0), the edge's strength \a A (1/m): without a
/// field α = A √((ħω - Eg)/1 eV) (1 eV/ħω), nought below the gap. With one,
/// the Franz-Keldysh effect (Callaway's, Tharmalingam's):
/// α = A (ħθ/1 eV)^½ π (Ai'(β)² - β Ai(β)²) (1 eV/ħω), β = (Eg - ħω)/ħθ,
/// ħθ = (q² ħ² F² / 2µ)^⅓ - a tail below the gap, ripples above it. With a
/// broadening \a sigma (J), the gap's spread as a Gaussian's (a tail even
/// without a field - Urbach's, as a real edge has; the ripples smoothed):
/// the mean of α over gaps Eg + s, s ~ N(0, σ²).
double franzKeldysh(double photon, double gap, double field, double mr, double A, double sigma = 0);

} // namespace qucs_s::fem

#endif
