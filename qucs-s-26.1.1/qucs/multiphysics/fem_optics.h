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

#include <complex>
#include <vector>

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

/// Graphene's surface conductivity (S) at angular frequency \a omega, its
/// chemical potential \a mu (J), its carriers' scattering time \a tau (s)
/// and temperature \a T (K), as e^{jωt} has it (σ' - jσ''): Kubo's formula in
/// Falkovsky's form (J. Phys.: Conf. Ser. 129, 012004, 2008) - the Drude
/// part D/(1/τ + jω), D = (2e²kT/πħ²) ln(2 cosh(µ/2kT)), and the interband
/// one (e²/4ħ) [G(ħω/2) - j (4ħω/π) ∫ (G(ε) - G(ħω/2))/((ħω)² - 4ε²) dε], G(ε)
/// = sinh(ε/kT)/(cosh(µ/kT) + cosh(ε/kT)): e²/4ħ (60.8 µS) where ħω > 2|µ|,
/// nearly none below (Pauli's blocking: an electro-absorption modulator's
/// off and on).
std::complex<double> grapheneConductivity(double omega, double mu, double tau, double T);

/// A direct-gap semiconductor's band filling (Burstein-Moss), after
/// Bennett, Soref and del Alamo (IEEE J. Quantum Electron. 26, 113, 1990):
/// parabolic bands, the heavy and light holes' absorption across the gap
/// α_j(E) = q² Ep (2 m_rj/ħ²)^{3/2} √(E - Eg) / (12π n c ε0 m0 ω) (Kane's Ep,
/// |ê·p|² = m0 Ep/6; or \a A, the edge's strength as franzKeldysh() takes it,
/// shared by their reduced masses' m_r^{3/2}), bleached where free carriers
/// fill the states it joins - Fermi-Dirac's at \a T: Δα(E) = -Σ_j α_j(E)
/// (f_c(E_cj) + f_v(E_vj)), E_cj = (E - Eg) m_rj/me, E_vj = (E - Eg) m_rj/m_j -
/// and the index Kramers-Kronig's of it, Δn(E) = (cħ/π) P∫ Δα(E')/(E'² - E²)
/// dE'. Energies in J, masses in m0.
struct BandFillingMedium {
    double Eg = 0, me = 0, mhh = 0, mlh = 0, Ep = 0, n = 1, T = 300;
    double A = 0;   ///< 1/m; nought: of Ep
};
/// Δn and Δα (1/m) at photon energy \a E: of \a carriers electrons per m³
/// (\a holes: holes), none of the other.
struct IndexChange {
    double dn = 0, dalpha = 0;
};
IndexChange bandFilling(const BandFillingMedium& m, double E, double carriers, bool holes);

/// The same tabulated at \a E over 1e20 to 1e26 per m³, and read at any
/// density: below the table's first, as N (non-degenerate: Boltzmann's).
class BandFillingTable
{
public:
    BandFillingTable() = default;
    BandFillingTable(const BandFillingMedium& m, double E, bool holes);
    IndexChange at(double carriers) const;
    bool valid() const { return !a_dn.empty(); }

private:
    std::vector<double> a_logN, a_dn, a_dalpha;
};

} // namespace qucs_s::fem

#endif
