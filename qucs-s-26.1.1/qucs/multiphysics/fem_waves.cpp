/*
 * fem_waves.cpp - electromagnetic waves: a cross-section's modes (the
 *                 fields e^{-jβz} along it, full-vector: Et on Nédélec's
 *                 edge elements, Ez on Lagrange's - Lee, Sun and Cendes'
 *                 formulation, A x = -β² B x, solved by a shift and invert
 *                 about the effective index guessed) and waves in the plane
 *                 (Ez or Hz: -∇·(P ∇u) - k0² q u = 0) with ports and their
 *                 S-parameters, scattering boundaries, a unit cell's
 *                 periodic (Floquet) condition and periodic ports - each
 *                 diffraction order's reflectance and transmittance; in media of a complex
 *                 refractive index or permittivity, anisotropic (diagonal),
 *                 warmed (dn/dT), and perfectly matched layers
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_edge.h"
#include "fem_materials.h"
#include "fem_optics.h"
#include "fem_problem.h"

#include <Eigen/Dense>

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>

namespace qucs_s::fem::detail {

namespace {

using cd = std::complex<double>;
constexpr double C0 = 299792458.0;
constexpr double Mu0 = 1.25663706127e-6;
const cd J(0, 1);

/// A complex 3x3's inverse (false: singular).
bool inverse3(const cd a[3][3], cd out[3][3])
{
    const cd det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                   + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (std::abs(det) < 1e-300) return false;
    out[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
    out[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
    out[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
    out[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
    out[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
    out[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
    out[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
    out[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
    out[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
    return true;
}

/// R T Rᵀ of a complex tensor (into the model's axes), and Rᵀ T R (into the crystal's).
void toModel(const Rotation& r, const cd in[3][3], cd out[3][3])
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            cd v = 0;
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) v += r.r[i][a] * r.r[j][b] * in[a][b];
            out[i][j] = v;
        }
}

void toCrystal(const Rotation& r, const double in[3][3], double out[3][3])
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double v = 0;
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) v += r.r[a][i] * r.r[b][j] * in[a][b];
            out[i][j] = v;
        }
}

/// Voigt's pairs (xx, yy, zz, yz, xz, xy).
constexpr int VoigtI[6] = {0, 1, 2, 1, 0, 0};
constexpr int VoigtJ[6] = {0, 1, 2, 2, 2, 1};

/// What a Media is set up with: its physics' compiling, its materials'
/// texts (a tensor's as it is) and orientations, where to say what is wrong.
using MediaContext = MediaHooks;

/// The media of a waves physics, by domain: what makes ε (complex, a
/// tensor) and µ (along x, y, z) at a point - a refractive index n - jκ
/// (along each axis; warmed: + dn/dT (T - Tref)), or a permittivity with
/// its loss tangent and conductivity, in a crystal's axes turned into the
/// model's by its orientation; Pockels' and the photoelastic effect's
/// Δ(1/ε), a garnet's gyrotropy; and the perfectly matched layers'
/// stretching.
class Media
{
public:
    struct Domain {
        bool defined = false;
        bool byIndex = true;
        // In the crystal's axes (a feature's own: the model's): n - jκ along
        // each, or εr's tensor (invalid: nought) with tan δ along each and σ.
        Expression n[3], kappa[3], eps[3][3], tand[3], sigma[3][3];
        Expression mur[3];          // the model's axes
        Rotation rotation;          // the crystal's axes in the model's
        Expression dndT[3], T, Tref;   // warmed: all three (dn/dT along each axis)
        // Free carriers, N and P (1/m³): the plasma dispersion (0 silicon's,
        // Soref and Bennett's; 1 Drude's of the masses (m0); 2 its
        // coefficients given), their absorption (cross-sections at 1550 nm,
        // as λ²), band filling (0 computed of its medium, its tables - at
        // n 1 - for the photon energy bfKey; 1 Δn per carrier given).
        Expression carriersN, carriersP;
        int plasmaModel = -1;
        double drudeMe = 0, drudeMh = 0;
        Expression dndN, dndP;
        bool fca = false;
        Expression sigmaN, sigmaP;
        int bandFilling = -1;
        bool bandHoles = true;
        BandFillingMedium bfMedium;
        Expression pbfN, pbfP;
        mutable double bfKey = NaN;
        mutable BandFillingTable bfElectrons, bfHoles;
        // Absorption across a direct gap and its change with a field (the
        // Franz-Keldysh effect): the field (V/m, a semiconductor's), the gap
        // (J, or eV as a number), the reduced mass (m0), the edge's strength (1/m).
        bool electroabsorption = false;
        Expression eaField, eaGap, eaMass, eaStrength, eaBroadening;
        // (Its value without a field, the same wherever the photon, gap,
        // mass, strength and broadening are: the last kept.)
        mutable double eaZeroKey[5] = {NaN, NaN, NaN, NaN, NaN};
        mutable double eaZero = 0;
        // Pockels': r (6x3) in the axes of rRotation, the static field E
        // (the model's axes).
        bool electrooptic = false;
        Expression r[6][3], field[3];
        Rotation rRotation;
        // The quadratic electro-optic effect (Kerr's, a well stack's QCSE):
        // Δn_k = ½ n³ R_k (|E|² - E0²), E the field above.
        bool kerr = false;
        Expression kerrR[3], kerrE0;
        // A quantum wells' absorption and index of the field along their
        // axis (qwAxis: 0 x, 1 y): the table its feature (qwFeature) is
        // given at the photon energy solved; the index's change from F0.
        QString qwFeature;
        Expression qwField, qwF0;
        int qwAxis = 1;
        std::shared_ptr<const WellTable> qwTable;
        // The photoelastic effect: p (6x6) in the axes of pRotation, the
        // strain (the model's axes, Voigt's engineering one).
        bool photoelastic = false;
        Expression p[6][6], strain[6];
        Rotation pRotation;
        // A garnet's gyrotropy: g along the model's axis gAxis.
        bool magnetooptic = false;
        int gAxis = 0;
        Expression g;
        bool pml = false;
        // Where its material's index holds (its nrange; none: anywhere), and
        // whose.
        double from = 0, to = std::numeric_limits<double>::infinity();
        QString rangeOf;
        // A PML's stretching: along x where it is left or right of the inner
        // region [x0, x1], growing to its own edge there; along y likewise
        // (a frame round the region: both at its corners).
        double inner[4] = {NaN, NaN, NaN, NaN};   // x0, x1, y0, y1 (m)
        double outer[4] = {NaN, NaN, NaN, NaN};
    };
    std::vector<Domain> domains;
    mutable QSet<QString> warned;   // (the materials said out of their range)
    /// A body of revolution's (x r, y z, the third axis φ): its layers'
    /// stretching a cylinder's - r̃ = ∫ s_r, Λ = ((r̃/r) s_z/s_r, (r̃/r)
    /// s_r/s_z, (r/r̃) s_r s_z) along r, z, φ.
    bool cylindrical = false;

    /// What is said where an index is taken at \a omega outside where its
    /// material holds it - each material once.
    QStringList outOfRange(double omega) const
    {
        QStringList out;
        if (!(omega > 0)) return out;
        const double lambda = 2 * M_PI * C0 / omega;
        for (const Domain& m : domains) {
            if (!m.defined || m.rangeOf.isEmpty() || (lambda >= m.from * (1 - 1e-9) && lambda <= m.to * (1 + 1e-9)) || warned.contains(m.rangeOf))
                continue;
            warned.insert(m.rangeOf);
            out << tr("Warning: %1's index holds from %2 to %3 µm (as its material gives it); here it is taken at %4 µm - give it for "
                      "this wavelength")
                       .arg(m.rangeOf, formatNumber(m.from * 1e6, 4), formatNumber(m.to * 1e6, 4), formatNumber(lambda * 1e6, 4));
        }
        return out;
    }

    /// ε (a tensor, the model's axes) and µ (along x, y, z) at the point
    /// the evaluator is at, at angular frequency \a omega; \a k0 for the
    /// PML's stretching.
    struct At {
        cd epsT[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        cd eps[3] = {1, 1, 1};   // its diagonal
        cd mu[3] = {1, 1, 1};
        bool ok = false;
        /// Its xz, yz, zx or zy not nought: the transverse field coupled to
        /// Ez (a mode found by β, not β²).
        bool coupled = false;
        // (By index: n, and of κ what is absorbed across the gap - the
        // medium's own and the direct gap's -, the direct gap's alone and
        // its value without a field; not the free carriers'.)
        double n = 1, kappaGap = 0, kappaEA = 0, kappaEA0 = 0;
        // The free carriers' Δn and absorption (1/m, net of band filling's bleaching).
        double dnCarriers = 0, alphaCarriers = 0;
        // Quantum wells' (TE, TM): Δn from F0 and absorption (1/m).
        double dnWells[2] = {0, 0}, alphaWells[2] = {0, 0};
    };
    At at(PointEvaluator& e, int d, double omega, double k0) const
    {
        At out;
        if (d < 0 || d >= int(domains.size()) || !domains[std::size_t(d)].defined) return out;
        const Domain& m = domains[std::size_t(d)];
        auto value = [&](const Expression& x, double fallback) {
            if (!x.isValid()) return fallback;
            const double v = x.isConstant() ? x.constant() : e.eval(x);
            return std::isfinite(v) ? v : fallback;
        };
        double warm[3] = {0, 0, 0};
        if (m.dndT[0].isValid() && m.T.isValid()) {
            const double dT = value(m.T, 0) - value(m.Tref, 293.15);
            for (int k = 0; k < 3; ++k) warm[k] = value(m.dndT[k].isValid() ? m.dndT[k] : m.dndT[0], 0) * dT;
        }
        // Free carriers: their index (plasma dispersion, band filling) and
        // absorption, at the wavelength solved.
        double plasma = 0, plasmaKappa = 0, bleachKappa = 0;
        const bool carriers = m.carriersN.isValid() || m.carriersP.isValid();
        const double lambda = omega > 0 ? 2 * M_PI * 299792458.0 / omega : 1.55e-6;
        if (carriers) {
            const double scale = (lambda / 1.55e-6) * (lambda / 1.55e-6);
            const double N = std::max(0.0, value(m.carriersN, 0)), P = std::max(0.0, value(m.carriersP, 0));   // (1/m³)
            const double n0 = std::max(1.0, value(m.n[0], 1.0));
            double alpha = 0;   // (1/m)
            if (m.plasmaModel == 0) {
                // Silicon's: Soref and Bennett's at 1550 nm (cm⁻³, cm⁻¹), as λ².
                const double Ncm = N * 1e-6, Pcm = P * 1e-6;
                plasma -= (8.8e-22 * Ncm + 8.5e-18 * std::pow(Pcm, 0.8)) * scale;
                alpha += (8.5e-18 * Ncm + 6.0e-18 * Pcm) * scale * 100;
            } else if (m.plasmaModel == 1) {
                // Drude's: -(q²λ²/8π²c²ε0 n)(N/me + P/mh).
                constexpr double q = 1.602176634e-19, m0 = 9.1093837015e-31, eps0 = 8.8541878188e-12;
                const double k = q * q * lambda * lambda / (8 * M_PI * M_PI * C0 * C0 * eps0 * n0 * m0);
                plasma -= k * (N / m.drudeMe + P / m.drudeMh);
            } else if (m.plasmaModel == 2) {
                plasma += value(m.dndN, 0.0) * N + value(m.dndP, 0.0) * P;
            }
            if (m.fca) alpha += (value(m.sigmaN, 0.0) * N + value(m.sigmaP, 0.0) * P) * scale;
            double bleach = 0;
            if (m.bandFilling == 0 && omega > 0) {
                // (Its tables at n 1: of Kane's Ep its absorption is as 1/n.)
                const double photon = 1.054571817e-34 * omega;
                if (m.bfKey != photon) {
                    m.bfElectrons = BandFillingTable(m.bfMedium, photon, false);
                    m.bfHoles = m.bandHoles ? BandFillingTable(m.bfMedium, photon, true) : BandFillingTable();
                    m.bfKey = photon;
                }
                const double per = m.bfMedium.A > 0 ? 1 : 1 / n0;
                const IndexChange e = m.bfElectrons.at(N), h = m.bandHoles ? m.bfHoles.at(P) : IndexChange();
                plasma += (e.dn + h.dn) * per;
                bleach += (e.dalpha + h.dalpha) * per;   // (nought or less: the edge's absorption bleached)
            } else if (m.bandFilling == 1) {
                plasma += value(m.pbfN, 0.0) * N + value(m.pbfP, 0.0) * P;
            }
            plasmaKappa = alpha * lambda / (4 * M_PI);
            bleachKappa = -bleach * lambda / (4 * M_PI);
        }
        // A direct gap's absorption at the field there.
        if (m.electroabsorption && omega > 0) {
            const double photon = 1.054571817e-34 * omega;
            // (Energies in J, or eV as a number or in V.)
            auto energy = [&](const Expression& x, double fallback) {
                const double v = value(x, fallback);
                return x.isValid() && x.dim() != Dim::of(2, 1, -2) ? v * 1.602176634e-19 : v;
            };
            const double gap = energy(m.eaGap, NaN), sigma = std::max(energy(m.eaBroadening, 0.0), 0.0);
            const double mr = value(m.eaMass, NaN), A = value(m.eaStrength, NaN), F = value(m.eaField, 0.0);
            if (std::isfinite(gap) && mr > 0 && A > 0) {
                // κ = α λ/4π = α c/2ω.
                out.kappaEA = franzKeldysh(photon, gap, F, mr, A, sigma) * C0 / (2 * omega);
                const double key[5] = {photon, gap, mr, A, sigma};
                if (!std::equal(key, key + 5, m.eaZeroKey)) {
                    std::copy(key, key + 5, m.eaZeroKey);
                    m.eaZero = franzKeldysh(photon, gap, 0, mr, A, sigma);
                }
                out.kappaEA0 = m.eaZero * C0 / (2 * omega);
            }
        }
        // The quadratic electro-optic effect along each axis.
        double kerr[3] = {0, 0, 0};
        if (m.kerr) {
            const double n0 = value(m.n[0], 1.0);
            double E2 = 0;
            for (int k = 0; k < 3; ++k) E2 += std::pow(value(m.field[k], 0.0), 2);
            const double E0 = value(m.kerrE0, 0.0);
            for (int k = 0; k < 3; ++k) {
                const double nk = value(m.n[k].isValid() ? m.n[k] : m.n[0], n0);
                kerr[k] = 0.5 * nk * nk * nk * value(m.kerrR[k].isValid() ? m.kerrR[k] : m.kerrR[0], 0.0) * (E2 - E0 * E0);
            }
        }
        // A quantum wells' absorption and index at the field there: TE in
        // their plane, TM along their axis.
        double wellsN[3] = {0, 0, 0}, wellsKappa[3] = {0, 0, 0};
        if (m.qwTable) {
            double now[4], given[4];
            m.qwTable->at(value(m.qwField, 0.0), now);
            m.qwTable->at(value(m.qwF0, 0.0), given);
            for (int k = 0; k < 3; ++k) {
                const int tm = k == m.qwAxis ? 1 : 0;
                wellsN[k] = now[2 + tm] - given[2 + tm];
                wellsKappa[k] = std::max(0.0, now[tm]) * lambda / (4 * M_PI);
            }
            for (int pol = 0; pol < 2; ++pol) {
                out.dnWells[pol] = now[2 + pol] - given[2 + pol];
                out.alphaWells[pol] = std::max(0.0, now[pol]);
            }
        }
        if (carriers) {
            // (Bleached at most of what the medium absorbs there: no gain of it.)
            if (bleachKappa > 0) plasmaKappa -= std::min(bleachKappa, std::max(0.0, value(m.kappa[0], 0.0) + out.kappaEA));
            out.dnCarriers = plasma;
            out.alphaCarriers = plasmaKappa * 4 * M_PI / lambda;
        }
        // ε in the crystal's axes.
        cd ec[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        if (m.byIndex) {
            // (The wells' absorption across their gap: κ across it, as the
            // direct gap's - TE's, along x.)
            out.n = value(m.n[0], 1.0) + warm[0] + plasma + kerr[0] + wellsN[0];
            out.kappaGap = value(m.kappa[0], 0.0) + out.kappaEA + wellsKappa[0];
            for (int k = 0; k < 3; ++k) {
                const cd n(value(m.n[k].isValid() ? m.n[k] : m.n[0], 1.0) + warm[k] + plasma + kerr[k] + wellsN[k],
                           -value(m.kappa[k].isValid() ? m.kappa[k] : m.kappa[0], 0.0) - plasmaKappa - out.kappaEA - wellsKappa[k]);
                ec[k][k] = n * n;
            }
        } else {
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    const double er = i == j ? value(m.eps[i][i].isValid() ? m.eps[i][i] : m.eps[0][0], 1.0) : value(m.eps[i][j], 0.0);
                    if (er == 0) continue;
                    // (tan δ along each axis where εr is diagonal; one alike.)
                    const double tand = i == j ? value(m.tand[i].isValid() ? m.tand[i] : m.tand[0], 0.0) : value(m.tand[0], 0.0);
                    ec[i][j] = cd(er, -er * tand);
                }
            if (omega > 0)
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) {
                        const double sigma = value(m.sigma[i][j], 0.0);
                        if (sigma != 0) ec[i][j] -= J * sigma / (omega * 8.8541878188e-12);
                    }
        }
        toModel(m.rotation, ec, out.epsT);
        // Pockels' and the photoelastic Δ(1/ε): ε = (ε⁻¹ + Δη)⁻¹.
        if (m.electrooptic || m.photoelastic) {
            double deta[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
            if (m.electrooptic) {
                const double Em[3] = {value(m.field[0], 0.0), value(m.field[1], 0.0), value(m.field[2], 0.0)};
                double Ec[3];
                m.rRotation.toCrystal(Em, Ec);
                double dc[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
                for (int I = 0; I < 6; ++I) {
                    double v = 0;
                    for (int k = 0; k < 3; ++k) v += value(m.r[I][k], 0.0) * Ec[k];
                    dc[VoigtI[I]][VoigtJ[I]] = dc[VoigtJ[I]][VoigtI[I]] = v;
                }
                cd in[3][3], dm[3][3];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) in[i][j] = dc[i][j];
                toModel(m.rRotation, in, dm);
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) deta[i][j] += dm[i][j].real();
            }
            if (m.photoelastic) {
                // The strain into the crystal's axes (a tensor's, half the
                // engineering shears), p S there, back.
                double sm[3][3], sc[3][3];
                for (int I = 0; I < 6; ++I) {
                    const double v = value(m.strain[I], 0.0) * (I < 3 ? 1.0 : 0.5);
                    sm[VoigtI[I]][VoigtJ[I]] = sm[VoigtJ[I]][VoigtI[I]] = v;
                }
                toCrystal(m.pRotation, sm, sc);
                double S[6];
                for (int I = 0; I < 6; ++I) S[I] = sc[VoigtI[I]][VoigtJ[I]] * (I < 3 ? 1.0 : 2.0);
                cd dc[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}, dm[3][3];
                for (int I = 0; I < 6; ++I) {
                    double v = 0;
                    for (int J2 = 0; J2 < 6; ++J2) v += value(m.p[I][J2], 0.0) * S[J2];
                    dc[VoigtI[I]][VoigtJ[I]] = dc[VoigtJ[I]][VoigtI[I]] = v;
                }
                toModel(m.pRotation, dc, dm);
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) deta[i][j] += dm[i][j].real();
            }
            cd eta[3][3];
            if (inverse3(out.epsT, eta)) {
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) eta[i][j] += deta[i][j];
                inverse3(eta, out.epsT);
            }
        }
        // A garnet's gyrotropy: ε_ij + j e_ijk g_k (e^{jωt}).
        if (m.magnetooptic) {
            const double g = value(m.g, 0.0);
            const int k = m.gAxis, a = (k + 1) % 3, b = (k + 2) % 3;
            out.epsT[a][b] += J * g;
            out.epsT[b][a] -= J * g;
        }
        const double mur[3] = {value(m.mur[0], 1.0), value(m.mur[1].isValid() ? m.mur[1] : m.mur[0], 1.0), value(m.mur[2].isValid() ? m.mur[2] : m.mur[0], 1.0)};
        for (int k = 0; k < 3; ++k) {
            out.mu[k] = mur[k];
            out.eps[k] = out.epsT[k][k];
        }
        if (m.pml) {
            // Stretched coordinates: x' = ∫ sx, its ε and µ (sx sy) S⁻¹ T
            // S⁻¹ times the medium's, S = diag(sx, sy, 1).
            const QPointF p = e.point();
            auto stretch = [&](double v, int axis) -> cd {
                const double lo = m.inner[2 * axis], hi = m.inner[2 * axis + 1];
                const double outLo = m.outer[2 * axis], outHi = m.outer[2 * axis + 1];
                double depth = 0, xi = 0;
                if (v > hi && outHi > hi) {
                    depth = outHi - hi;
                    xi = (v - hi) / depth;
                } else if (v < lo && outLo < lo) {
                    depth = lo - outLo;
                    xi = (lo - v) / depth;
                } else
                    return 1.0;
                xi = std::clamp(xi, 0.0, 1.0);
                const double kk = k0 * std::sqrt(std::max(1.0, std::abs(out.epsT[2][2].real() * out.mu[2].real())));
                // A reflection of e^-13.8 at its outside: 2 k σ0 d / 3 = 13.8.
                const double sigma0 = 20.7 / (kk * depth);
                return cd(1.0, -sigma0 * xi * xi);
            };
            const cd s[3] = {stretch(p.x(), 0), stretch(p.y(), 1), 1.0};
            if (cylindrical) {
                // r̃: ∫ s_r from the inner region's edge, -j σ0 d ξ³/3 more.
                const double r = p.x(), hi = m.inner[1], depth = m.outer[1] - m.inner[1];
                cd rt = r;
                if (r > hi && depth > 0) {
                    const double xi = std::clamp((r - hi) / depth, 0.0, 1.0);
                    rt = r + (s[0] - 1.0) * depth * xi / 3.0;   // (s_r - 1 = -j σ0 ξ²)
                }
                const cd ratio = r > 0 ? rt / r : 1.0;
                const cd f[3] = {ratio * s[1] / s[0], ratio * s[0] / s[1], s[0] * s[1] / ratio};
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) out.epsT[i][j] *= i == j ? f[i] : 1.0;
                for (int k = 0; k < 3; ++k) {
                    out.mu[k] *= f[k];
                    out.eps[k] = out.epsT[k][k];
                }
            } else {
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) out.epsT[i][j] *= s[0] * s[1] / (s[i] * s[j]);
                const cd f[3] = {s[1] / s[0], s[0] / s[1], s[0] * s[1]};
                for (int k = 0; k < 3; ++k) {
                    out.mu[k] *= f[k];
                    out.eps[k] = out.epsT[k][k];
                }
            }
        }
        const double scale = std::abs(out.eps[0]) + std::abs(out.eps[1]) + std::abs(out.eps[2]);
        for (int i = 0; i < 2; ++i)
            out.coupled = out.coupled || std::abs(out.epsT[i][2]) > 1e-12 * scale || std::abs(out.epsT[2][i]) > 1e-12 * scale;
        out.ok = true;
        return out;
    }

    /// Set up from \a node's features (a wave equation's, PMLs) on its
    /// domains; what is wrong to the context's problem.
    void setup(Solution& s, const Node& node, const std::vector<char>& on, const std::vector<const Node*>& material, const MediaContext& c)
    {
        const Topology& topo = s.topology();
        const std::size_t nd = topo.domains.size();
        domains.assign(nd, Domain());
        warned.clear();
        std::vector<const Node*> modelOf(nd, nullptr);
        std::vector<char> pml(nd, 0);
        QString why;
        for (const Node& feature : node.children) {
            if (!feature.enabled) continue;
            const NodeKind* k = feature.kind();
            if (!k || k->selection != Level::Domain) continue;
            const QVector<int> picked = resolveSelection(topo, s.model(), Level::Domain, feature.selection(), &why);
            if (!why.isEmpty()) c.problem(QStringLiteral("%1: %2").arg(feature.name(), why));
            if (feature.type.endsWith(QLatin1String("/wave")))
                for (int d : picked) modelOf[std::size_t(d)] = &feature;
            else if (feature.type.endsWith(QLatin1String("/pml")))
                for (int d : picked) pml[std::size_t(d)] = 1;
        }
        // The region the PMLs surround: the other domains' bounds.
        QRectF inner;
        for (std::size_t d = 0; d < nd; ++d)
            if (on[d] && !pml[d]) inner = inner.isNull() ? topo.domains[d].bounds : inner.united(topo.domains[d].bounds);
        for (std::size_t d = 0; d < nd; ++d) {
            if (!on[d]) continue;
            Domain& m = domains[d];
            const Node* feat = modelOf[d];
            if (!feat) {
                c.problem(tr("domain %1 has no Wave Equation feature").arg(d + 1));
                continue;
            }
            const bool user = feat->text(QStringLiteral("source")) == QLatin1String("user");
            const Node* mat = material[d];
            const QString object = topo.objectNames.value(topo.domains[d].owner);
            if (!user && !mat) {
                c.problem(tr("domain %1 (%2) has no material: its refractive index or permittivity is needed").arg(d + 1).arg(object));
                continue;
            }
            const Node& from = user ? *feat : *mat;
            auto text = [&](const QString& key) { return user ? feat->text(key) : c.property(*mat, key); };
            // A property along the axes: one, or three (x; y; z) - the crystal's
            // for a material's, the model's for the feature's.
            auto axes = [&](const QString& key, Expression out[3]) {
                const QString t = text(key);
                if (t.trimmed().isEmpty()) return true;
                QString err;
                const std::optional<TensorText> tt = tensorText(t, &err, {1, 3});
                if (!tt) {
                    c.problem(tr("%1's %2: %3").arg(from.name(), key, err));
                    return false;
                }
                for (int k = 0; k < (tt->items == 1 ? 1 : 3); ++k) out[k] = c.compile(tt->c[k][k], from, key);
                return true;
            };
            m.rotation = user ? Rotation() : c.rotation(*mat);
            m.byIndex = feat->text(QStringLiteral("constitutive")) != QLatin1String("epsilonr");
            const QString aniso = user ? feat->text(QStringLiteral("anisotropy")) : QString();
            if (m.byIndex) {
                if (aniso == QLatin1String("tensor")) {
                    c.problem(tr("%1: a tensor is a permittivity's - its electric displacement by permittivity").arg(feat->name()));
                    continue;
                }
                if (aniso == QLatin1String("diagonal")) {
                    const char* keys[3] = {"nx", "ny", "nz"};
                    for (int k = 0; k < 3; ++k) m.n[k] = c.compile(feat->text(QString::fromLatin1(keys[k])), *feat, QString::fromLatin1(keys[k]));
                } else {
                    if (text(QStringLiteral("n")).trimmed().isEmpty()) {
                        c.problem(tr("domain %1 (%2): its material, %3, has no refractive index n").arg(d + 1).arg(object, mat ? mat->name() : QString()));
                        continue;
                    }
                    if (!axes(QStringLiteral("n"), m.n)) continue;
                }
                if (!axes(QStringLiteral("kappa"), m.kappa)) continue;
                // (Its wavelengths: "from to", lengths.)
                const QStringList range = user ? QStringList() : c.property(*mat, QStringLiteral("nrange")).simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
                if (range.size() == 2) {
                    const std::optional<double> a = evaluateConstant(range[0], s.parameters().scope, Dim::length());
                    const std::optional<double> b = evaluateConstant(range[1], s.parameters().scope, Dim::length());
                    if (a && b && *a > 0 && *b > *a) {
                        m.from = *a;
                        m.to = *b;
                        m.rangeOf = mat->name();
                    }
                }
            } else {
                bool diagonal = true;
                if (aniso == QLatin1String("diagonal") || aniso == QLatin1String("tensor")) {
                    const char* keys[3] = {"epsxx", "epsyy", "epszz"};
                    for (int k = 0; k < 3; ++k) m.eps[k][k] = c.compile(feat->text(QString::fromLatin1(keys[k])), *feat, QString::fromLatin1(keys[k]));
                    if (aniso == QLatin1String("tensor")) {
                        const char* off[3] = {"epsyz", "epsxz", "epsxy"};
                        for (int I = 3; I < 6; ++I) {
                            const QString t = feat->text(QString::fromLatin1(off[I - 3]));
                            if (t.trimmed().isEmpty() || t.trimmed() == QLatin1String("0")) continue;
                            m.eps[VoigtI[I]][VoigtJ[I]] = m.eps[VoigtJ[I]][VoigtI[I]] = c.compile(t, *feat, QString::fromLatin1(off[I - 3]));
                            diagonal = false;
                        }
                    }
                } else {
                    const QString er = text(QStringLiteral("epsilonr"));
                    if (er.trimmed().isEmpty()) {
                        c.problem(tr("domain %1 (%2): its material, %3, has no relative permittivity").arg(d + 1).arg(object, mat ? mat->name() : QString()));
                        continue;
                    }
                    QString err;
                    const std::optional<TensorText> t = tensorText(er, &err);
                    if (!t) {
                        c.problem(tr("%1's relative permittivity: %2").arg(from.name(), err));
                        continue;
                    }
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            if (t->c[i][j] != QLatin1String("0")) m.eps[i][j] = c.compile(t->c[i][j], from, QStringLiteral("epsilonr"));
                    diagonal = t->diagonal();
                }
                if (!axes(QStringLiteral("tand"), m.tand)) continue;
                if (!diagonal && m.tand[1].isValid()) {
                    c.problem(tr("%1: a loss tangent along the axes is a diagonal permittivity's (give one tan δ, or σ)").arg(from.name()));
                    continue;
                }
                const QString sg = text(QStringLiteral("sigma"));
                if (!sg.trimmed().isEmpty()) {
                    QString err;
                    const std::optional<TensorText> t = tensorText(sg, &err);
                    if (!t) {
                        c.problem(tr("%1's conductivity: %2").arg(from.name(), err));
                        continue;
                    }
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            if (t->c[i][j] != QLatin1String("0")) m.sigma[i][j] = c.compile(t->c[i][j], from, QStringLiteral("sigma"));
                }
            }
            // µr along the model's axes (a material's tensor turned: its
            // diagonal).
            const QString mur = text(QStringLiteral("mur"));
            if (!mur.trimmed().isEmpty()) {
                QString err;
                std::optional<TensorText> t = tensorText(mur, &err);
                if (!t) {
                    c.problem(tr("%1's relative permeability: %2").arg(from.name(), err));
                    continue;
                }
                if (!user) *t = rotated(*t, m.rotation);
                for (int k = 0; k < (t->items == 1 ? 1 : 3); ++k) m.mur[k] = c.compile(t->c[k][k], from, QStringLiteral("mur"));
            }
            // Free carriers: the plasma dispersion, their absorption, band filling.
            const bool plasma = feat->flag(QStringLiteral("plasma")), fca = feat->flag(QStringLiteral("fca")),
                       filling = feat->flag(QStringLiteral("bandfilling"));
            if (plasma || fca || filling) {
                if (!m.byIndex)
                    c.problem(tr("%1: the free carriers' index and absorption add to a refractive index (its constitutive relation n)").arg(feat->name()));
                m.carriersN = c.compile(feat->text(QStringLiteral("Ncarriers")), *feat, QStringLiteral("Ncarriers"));
                m.carriersP = c.compile(feat->text(QStringLiteral("Pcarriers")), *feat, QStringLiteral("Pcarriers"));
            }
            // A constant of the feature's (key) or else its material's (\a
            // of) - at temperature \a T (K; nought: its texts as they are).
            auto constant = [feat, mat, &c, &s, d, &object](const QString& key, const QString& of, const Dim& dim, const QString& what,
                                                             double T = 0) -> std::optional<double> {
                QString t = key.isEmpty() ? QString() : feat->text(key).trimmed();
                const Node* holder = feat;
                if (t.isEmpty() && mat) {
                    t = (T > 0 ? Problem::materialText(*mat, of, &s.parameters().scope, T) : c.property(*mat, of)).trimmed();
                    holder = mat;
                }
                if (t.isEmpty()) {
                    c.problem(tr("domain %1 (%2): %3 - its material, %4, has none (%5)").arg(d + 1).arg(object, what, mat ? mat->name() : tr("none"), of));
                    return std::nullopt;
                }
                QString err;
                const MaterialScope atT(s.parameters().scope, T > 0 ? T : 300);
                const std::optional<double> v = evaluateConstant(t, atT, dim, &err);
                if (!v) c.problem(tr("%1's %2: %3").arg(holder->name(), of, err));
                return v;
            };
            // The holes' masses (m0), of the material's Luttinger parameters.
            auto holeMasses = [&](double* mhh, double* mlh) {
                const std::optional<double> g1 = constant(QString(), QStringLiteral("gamma1"), Dim(), tr("its holes' masses (Luttinger's γ1)"));
                const std::optional<double> g2 = constant(QString(), QStringLiteral("gamma2"), Dim(), tr("its holes' masses (Luttinger's γ2)"));
                if (!g1 || !g2 || !(*g1 - 2 * *g2 > 0)) return false;
                *mhh = 1 / (*g1 - 2 * *g2);
                *mlh = 1 / (*g1 + 2 * *g2);
                return true;
            };
            if (plasma) {
                const QString model = feat->text(QStringLiteral("plasmamodel"));
                if (model == QLatin1String("drude")) {
                    m.plasmaModel = 1;
                    const std::optional<double> me = constant(QStringLiteral("mepl"), QStringLiteral("me"), Dim(), tr("Drude's electron mass"));
                    m.drudeMe = me.value_or(NaN);
                    if (!feat->text(QStringLiteral("mhpl")).trimmed().isEmpty()) {
                        m.drudeMh = constant(QStringLiteral("mhpl"), QStringLiteral("mhpl"), Dim(), tr("Drude's hole mass")).value_or(NaN);
                    } else {
                        double mhh = NaN, mlh = NaN;
                        if (holeMasses(&mhh, &mlh))
                            m.drudeMh = (std::pow(mhh, 1.5) + std::pow(mlh, 1.5)) / (std::sqrt(mhh) + std::sqrt(mlh));
                    }
                    if (!(m.drudeMe > 0) || !(m.drudeMh > 0)) {
                        if (me && !(m.drudeMe > 0)) c.problem(tr("%1: Drude's masses are more than nought").arg(feat->name()));
                        m.plasmaModel = -1;
                    }
                } else if (model == QLatin1String("user")) {
                    m.plasmaModel = 2;
                    m.dndN = c.compile(feat->text(QStringLiteral("dndN")), *feat, QStringLiteral("dndN"));
                    m.dndP = c.compile(feat->text(QStringLiteral("dndP")), *feat, QStringLiteral("dndP"));
                } else {
                    m.plasmaModel = 0;
                    if (fca)
                        c.problem(tr("%1: silicon's plasma dispersion carries its own absorption (Soref and Bennett's): with Free-carrier "
                                     "absorption too it would count twice - Drude's index with it, or silicon's alone")
                                      .arg(feat->name()));
                }
            }
            if (fca) {
                const bool own = feat->text(QStringLiteral("fcasource")) == QLatin1String("user");
                for (const auto& [key, member] : {std::pair{QStringLiteral("sigmaFCn"), &Domain::sigmaN}, std::pair{QStringLiteral("sigmaFCp"), &Domain::sigmaP}}) {
                    const QString t = own ? feat->text(key) : mat ? c.property(*mat, key) : QString();
                    if (t.trimmed().isEmpty()) {
                        c.problem(tr("domain %1 (%2): its material, %3, has no free-carrier absorption cross-section %4 (InP's family has; "
                                     "or give them on the feature)")
                                      .arg(d + 1)
                                      .arg(object, mat ? mat->name() : tr("none"), key));
                        continue;
                    }
                    m.*member = c.compile(t, own ? *feat : *mat, key);
                }
                m.fca = true;
            }
            if (filling) {
                if (feat->text(QStringLiteral("bfmodel")) == QLatin1String("user")) {
                    m.bandFilling = 1;
                    m.pbfN = c.compile(feat->text(QStringLiteral("Pbfn")), *feat, QStringLiteral("Pbfn"));
                    m.pbfP = c.compile(feat->text(QStringLiteral("Pbfp")), *feat, QStringLiteral("Pbfp"));
                } else if (!mat) {
                    c.problem(tr("%1: band filling is computed of a material's gap, masses and Ep - domain %2 has none").arg(feat->name()).arg(d + 1));
                } else {
                    BandFillingMedium& bm = m.bfMedium;
                    // (A direct gap's: germanium's Egd, else the gap.)
                    const QString gapKey = c.property(*mat, QStringLiteral("Egd")).trimmed().isEmpty() ? QStringLiteral("Eg") : QStringLiteral("Egd");
                    const std::optional<double> T = constant(QStringLiteral("Tbf"), QStringLiteral("Tbf"), Dim::temperature(), tr("band filling's temperature"));
                    const double atT = T && *T > 0 ? *T : 300;
                    const std::optional<double> eg = constant(QString(), gapKey, Dim::of(2, 1, -2), tr("band filling's gap"), atT);
                    const std::optional<double> me = constant(QString(), QStringLiteral("me"), Dim(), tr("band filling's electron mass"), atT);
                    double mhh = NaN, mlh = NaN;
                    const bool holes = holeMasses(&mhh, &mlh);
                    // Its absorption's strength: the material's edge (Afk, a
                    // measured one), else Kane's Ep - Sommerfeld's enhancement
                    // with it, of the material's εr, unless left out.
                    double A = 0, Ep = 0, epsr = 0;
                    if (!c.property(*mat, QStringLiteral("Afk")).trimmed().isEmpty()) {
                        A = constant(QString(), QStringLiteral("Afk"), Dim::of(-1), tr("band filling's edge strength")).value_or(0);
                    } else {
                        Ep = constant(QString(), QStringLiteral("Ep"), Dim::of(2, 1, -2), tr("band filling's absorption (Kane's Ep)")).value_or(0);
                        if (feat->flag(QStringLiteral("bfexciton")))
                            epsr = constant(QString(), QStringLiteral("epsilonr"), Dim(), tr("band filling's excitons (the static εr)")).value_or(0);
                    }
                    if (eg && me && T && holes && (A > 0 || Ep > 0)) {
                        bm.Eg = *eg;
                        bm.me = *me;
                        bm.mhh = mhh;
                        bm.mlh = mlh;
                        bm.T = *T;
                        bm.A = A;
                        bm.Ep = Ep;
                        bm.epsr = epsr;
                        m.bandFilling = 0;
                        m.bandHoles = feat->flag(QStringLiteral("bfholes"));
                    }
                }
            }
            if (feat->flag(QStringLiteral("electroabsorption"))) {
                if (!m.byIndex) c.problem(tr("%1: a direct gap's absorption adds to a refractive index (its constitutive relation n)").arg(feat->name()));
                // (The gap, mass and strength: the feature's own, or its material's.)
                const bool own = feat->text(QStringLiteral("easource")) == QLatin1String("user");
                for (const auto& [key, member] : {std::pair{QStringLiteral("Egd"), &Domain::eaGap}, std::pair{QStringLiteral("mr"), &Domain::eaMass},
                                                  std::pair{QStringLiteral("Afk"), &Domain::eaStrength}}) {
                    const QString t = own ? feat->text(key) : mat ? c.property(*mat, key) : QString();
                    if (t.trimmed().isEmpty()) {
                        c.problem(tr("domain %1 (%2): its material has no %3 for a direct gap's absorption (Germanium has: Egd, mr, Afk)")
                                      .arg(d + 1)
                                      .arg(object, key));
                        continue;
                    }
                    m.*member = c.compile(t, own ? *feat : *mat, key);
                }
                // (Its material's κ the same edge measured - the library's
                // semiconductors' -: the direct gap's in its place, not added
                // to it; a κ given in the feature added.)
                if (!user)
                    for (Expression& k : m.kappa) k = Expression();
                m.eaField = c.compile(feat->text(QStringLiteral("Fea")), *feat, QStringLiteral("Fea"));
                if (!feat->text(QStringLiteral("Ebroad")).trimmed().isEmpty())
                    m.eaBroadening = c.compile(feat->text(QStringLiteral("Ebroad")), *feat, QStringLiteral("Ebroad"));
                m.electroabsorption = true;
            }
            if (feat->flag(QStringLiteral("thermooptic"))) {
                const QString dndT = user ? feat->text(QStringLiteral("dndT")) : c.property(*mat, QStringLiteral("dndT"));
                if (dndT.trimmed().isEmpty()) c.problem(tr("domain %1 (%2): its material has no thermo-optic coefficient dn/dT").arg(d + 1).arg(object));
                else {
                    if (!axes(QStringLiteral("dndT"), m.dndT)) continue;
                    m.T = c.compile(feat->text(QStringLiteral("T")), *feat, QStringLiteral("T"));
                    m.Tref = c.compile(feat->text(QStringLiteral("Tref")), *feat, QStringLiteral("Tref"));
                }
            }
            if (feat->flag(QStringLiteral("electrooptic"))) {
                // r: the feature's (the model's axes) or its material's (its
                // crystal's, turned).
                const QString rt = user ? feat->text(QStringLiteral("r")) : c.property(*mat, QStringLiteral("r"));
                QString err;
                const std::optional<VoigtText> r = rt.trimmed().isEmpty() ? std::nullopt : voigtText(rt, QLatin1Char('r'), 6, 3, false, &err);
                if (!r) {
                    c.problem(rt.trimmed().isEmpty() ? tr("domain %1 (%2): its material has no electro-optic coefficients r").arg(d + 1).arg(object)
                                                     : tr("%1's electro-optic coefficients: %2").arg(from.name(), err));
                    continue;
                }
                bool inMetresPerVolt = true;
                for (int I = 0; I < 6; ++I)
                    for (int k = 0; k < 3; ++k)
                        if (r->c[I][k] != QLatin1String("0")) {
                            m.r[I][k] = c.compile(r->c[I][k], from, QStringLiteral("r"));
                            if (m.r[I][k].isValid() && !(m.r[I][k].dim() == Dim::of(-1, -1, 3, 1))) inMetresPerVolt = false;
                        }
                if (!inMetresPerVolt) {
                    c.problem(tr("%1's electro-optic coefficients are in m/V (pm/V): r33=30.8[pm/V]").arg(from.name()));
                    continue;
                }
                const char* keys[3] = {"Ex", "Ey", "Ez"};
                for (int k = 0; k < 3; ++k) m.field[k] = c.compile(feat->text(QString::fromLatin1(keys[k])), *feat, QString::fromLatin1(keys[k]));
                m.rRotation = m.rotation;
                m.electrooptic = true;
            }
            if (feat->flag(QStringLiteral("kerr"))) {
                if (!m.byIndex) c.problem(tr("%1: the quadratic electro-optic effect adds to a refractive index (its constitutive relation n)").arg(feat->name()));
                const QStringList items = feat->text(QStringLiteral("Rkerr")).split(QLatin1Char(';'), Qt::SkipEmptyParts);
                if (items.size() != 1 && items.size() != 3) {
                    c.problem(tr("%1: R along x, y, z - one value or three").arg(feat->name()));
                    continue;
                }
                for (int k = 0; k < int(items.size()); ++k) {
                    m.kerrR[k] = c.compile(items[k].trimmed(), *feat, QStringLiteral("Rkerr"));
                    if (m.kerrR[k].isValid() && !(m.kerrR[k].dim() == Dim::of(-2, -2, 6, 2) || m.kerrR[k].dim().isNone()))
                        c.problem(tr("%1: R is in m²/V² (1e-19[m^2/V^2])").arg(feat->name()));
                }
                m.kerrE0 = c.compile(feat->text(QStringLiteral("E0kerr")), *feat, QStringLiteral("E0kerr"));
                const char* keys[3] = {"Ex", "Ey", "Ez"};
                for (int k = 0; k < 3; ++k) m.field[k] = c.compile(feat->text(QString::fromLatin1(keys[k])), *feat, QString::fromLatin1(keys[k]));
                m.kerr = true;
            }
            if (feat->flag(QStringLiteral("qwabsorption"))) {
                if (!m.byIndex) c.problem(tr("%1: the wells' absorption adds to a refractive index (its constitutive relation n)").arg(feat->name()));
                const QString wells = feat->text(QStringLiteral("qwtag")).trimmed();
                const Node* qw = nullptr;
                for (const Node* p : s.model().physics())
                    if (p->enabled && p->tag == wells && p->type == QLatin1String("quantum")) qw = p;
                if (!qw) {
                    c.problem(tr("%1: there is no quantum wells physics %2 (its tag)").arg(feat->name(), wells));
                    continue;
                }
                m.qwFeature = feat->tag;
                m.qwAxis = qw->text(QStringLiteral("axis")) == QLatin1String("x") ? 0 : 1;
                m.qwField = c.compile(feat->text(QStringLiteral("Fqw")), *feat, QStringLiteral("Fqw"));
                m.qwF0 = c.compile(feat->text(QStringLiteral("F0qw")), *feat, QStringLiteral("F0qw"));
                // (Its material's κ a bulk edge - a compound's -: the wells'
                // own absorption in its place, as a direct gap's is.)
                if (!user)
                    for (Expression& k : m.kappa) k = Expression();
            }
            if (feat->flag(QStringLiteral("photoelastic"))) {
                const QString pt = user ? feat->text(QStringLiteral("p")) : c.property(*mat, QStringLiteral("p"));
                QString err;
                const std::optional<VoigtText> p = pt.trimmed().isEmpty() ? std::nullopt : voigtText(pt, QLatin1Char('p'), 6, 6, true, &err);
                if (!p) {
                    c.problem(pt.trimmed().isEmpty() ? tr("domain %1 (%2): its material has no photoelastic coefficients p").arg(d + 1).arg(object)
                                                     : tr("%1's photoelastic coefficients: %2").arg(from.name(), err));
                    continue;
                }
                for (int I = 0; I < 6; ++I)
                    for (int J2 = 0; J2 < 6; ++J2)
                        if (p->c[I][J2] != QLatin1String("0")) m.p[I][J2] = c.compile(p->c[I][J2], from, QStringLiteral("p"));
                const QString tag = feat->text(QStringLiteral("strain")).trimmed().isEmpty() ? QStringLiteral("solid") : feat->text(QStringLiteral("strain")).trimmed();
                const QString parts[6] = {QStringLiteral("ex"), QStringLiteral("ey"), QStringLiteral("ez"), QString(), QString(), QStringLiteral("exy")};
                for (int I = 0; I < 6; ++I)
                    if (!parts[I].isEmpty())
                        m.strain[I] = c.compile((I == 5 ? QStringLiteral("2*") : QString()) + tag + QLatin1Char('.') + parts[I], *feat, QStringLiteral("strain"));
                m.pRotation = m.rotation;
                m.photoelastic = true;
            }
            if (feat->flag(QStringLiteral("magnetooptic"))) {
                const QString g = user ? feat->text(QStringLiteral("g")) : c.property(*mat, QStringLiteral("g"));
                if (g.trimmed().isEmpty()) {
                    c.problem(tr("domain %1 (%2): its material has no gyration g").arg(d + 1).arg(object));
                    continue;
                }
                m.g = c.compile(g, from, QStringLiteral("g"));
                const QString axis = feat->text(QStringLiteral("magnetization"));
                m.gAxis = axis == QLatin1String("y") ? 1 : axis == QLatin1String("z") ? 2 : 0;
                m.magnetooptic = true;
            }
            m.defined = true;
            if (pml[d]) {
                m.pml = true;
                const QRectF b = topo.domains[d].bounds;
                const double scale = topo.unitScale;
                // It stretches where it reaches past the inner region.
                bool beyond = false;
                if (!inner.isNull()) {
                    const double tol = 1e-9 * std::max(inner.width(), inner.height());
                    beyond = b.left() < inner.left() - tol || b.right() > inner.right() + tol || b.top() < inner.top() - tol ||
                             b.bottom() > inner.bottom() + tol;
                    const double in[4] = {inner.left(), inner.right(), inner.top(), inner.bottom()};
                    const double out[4] = {b.left(), b.right(), b.top(), b.bottom()};
                    for (int k = 0; k < 4; ++k) {
                        m.inner[k] = in[k] * scale;
                        m.outer[k] = out[k] * scale;
                    }
                }
                if (!beyond)
                    c.problem(tr("domain %1: a perfectly matched layer lies outside the other domains - beside, above or round them").arg(d + 1));
            }
        }
    }
    /// The largest refractive index (real) on these domains, at their middles.
    double largestIndex(Solution& s, const std::vector<char>& on, double omega) const
    {
        double best = 1;
        PointEvaluator e(s);
        const Mesh& mesh = s.mesh();
        for (int t = 0; t < int(mesh.triangles.size()); ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (d < 0 || !on[std::size_t(d)] || domains[std::size_t(d)].pml) continue;
            e.moveTo(t, 1.0 / 3, 1.0 / 3);
            const At a = at(e, d, omega, 1);
            for (int k = 0; k < 3; ++k) best = std::max(best, std::sqrt(std::abs((a.eps[k] * a.mu[k]).real())));
        }
        return best;
    }
};

// ------------------------------------------------------------------ Modes

/// A conductive sheet on boundaries: graphene's (Kubo's of µc, τ, T) or
/// σs as given (σs' - j σs'').
struct Sheet {
    bool graphene = true;
    Expression muc, tau, T, re, im;
    QString name;
};

/// A sheet's σs where the evaluator is, at angular frequency \a omega.
cd sheetConductivity(PointEvaluator& e, const Sheet& s, double omega)
{
    auto value = [&](const Expression& x, double fallback) {
        if (!x.isValid()) return fallback;
        const double v = x.isConstant() ? x.constant() : e.eval(x);
        return std::isfinite(v) ? v : fallback;
    };
    if (!s.graphene) return cd(value(s.re, 0.0), -value(s.im, 0.0));
    // (µc in J, or eV as a number or in V.)
    double mu = value(s.muc, 0.0);
    if (s.muc.isValid() && s.muc.dim() != Dim::of(2, 1, -2)) mu *= 1.602176634e-19;
    return grapheneConductivity(omega, mu, value(s.tau, 1e-13), value(s.T, 300.0));
}

class ModeProblem : public Problem
{
public:
    ModeProblem(Solution& solution, int field) : Problem(solution, field) {}

    bool setup(QString* error) override;
    void initialize() override
    {
        Field& f = field();
        f.values.assign(std::size_t(2 * f.edges->size() + 2 * f.space->size()), 0.0);
        f.extra.assign(3, 0.0);
    }
    bool solve(const SolveOptions&, QString* error) override
    {
        *error = tr("%1: a mode analysis is solved by a Mode Analysis step").arg(a_node->name());
        return false;
    }
    bool modal() const override { return true; }
    int solveModes(const SolveOptions& options, int count, double guess, QString* error) override;
    void selectMode(int k) override;
    int chosenMode() const override { return a_chosen; }
    void setGlobals() override;
    std::vector<WellRequest> wellRequests() const override
    {
        // (Each wave feature's of its Quantum Wells' Absorption, once.)
        std::vector<WellRequest> out;
        for (const Node& feature : a_node->children) {
            if (!feature.enabled || !feature.type.endsWith(QLatin1String("/wave")) || !feature.flag(QStringLiteral("qwabsorption"))) continue;
            WellRequest w;
            w.feature = feature.tag;
            w.wells = feature.text(QStringLiteral("qwtag")).trimmed();
            w.spectrum = feature.text(QStringLiteral("spectrumstep")).trimmed();
            w.Fmax = evaluateConstant(feature.text(QStringLiteral("Fmax")), a_s.parameters().scope, Dim::of(1, 1, -3, -1)).value_or(NaN);
            w.Fpoints = feature.integer(QStringLiteral("Fpoints"));
            out.push_back(w);
        }
        return out;
    }
    void setWellTable(const QString& feature, std::shared_ptr<const WellTable> table) override
    {
        for (Media::Domain& m : a_media.domains)
            if (m.qwFeature == feature) m.qwTable = table;
    }

private:
    Media a_media;
    std::vector<int> a_edgeEq, a_nodeEq;   // equations, -1 none (fixed or not here)
    int a_nEdge = 0, a_n = 0;
    struct Mode {
        cd beta;
        Eigen::VectorXcd x;   // et then ez, by equation
        double share = NaN;   // of its |E|² in the domains tracked
        double te = NaN;      // of its transverse |E|², the part along x
    };
    std::vector<Mode> a_modes;
    int a_chosen = 0;   // of a_modes, the one its options choose
    bool a_tracking = false;
    double a_k0 = 0;
    int a_selected = -1;
    // The mode selected: its power absorbed across the gap, and what the
    // field adds to it, per metre (1/m); whether a medium has a direct gap's.
    double a_absorbedGap = 0, a_absorbedField = 0;
    bool a_electroabsorption = false;
    // Conductive sheets, and each boundary's (-1 none); what the mode
    // selected leaves in them, per metre (W/m at 1 W).
    std::vector<Sheet> a_sheets;
    std::vector<int> a_sheetOf;
    double a_sheetPower = 0;
};

bool ModeProblem::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field& f = field();
    const Topology& topo = a_s.topology();
    QString why;
    const QVector<int> domains = resolveSelection(topo, a_s.model(), Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(topo.domains.size(), 0);
    for (int d : domains) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    a_media.setup(a_s, *a_node, f.domains, materials(), mediaHooks());
    a_electroabsorption = false;
    for (const Media::Domain& m : a_media.domains) a_electroabsorption = a_electroabsorption || (m.defined && m.electroabsorption);
    // For the variables: the index (its real part, as asked) by domain.
    Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    coef.c.assign(topo.domains.size(), Expression());
    std::vector<Expression> mur(topo.domains.size());
    for (std::size_t d = 0; d < a_media.domains.size(); ++d) {
        if (a_media.domains[d].defined && a_media.domains[d].byIndex) coef.c[d] = a_media.domains[d].n[0];
        mur[d] = a_media.domains[d].mur[0];
    }
    coef.named[QStringLiteral("mur")] = mur;
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    // The equations: edges and nodes of these domains, those on a perfect
    // electric conductor (the outside, by default) fixed at nought.
    const EdgeSpace& es = *f.edges;
    const Space& ns = *f.space;
    const Mesh& mesh = a_s.mesh();
    a_edgeEq.assign(std::size_t(es.size()), -1);
    a_nodeEq.assign(std::size_t(ns.size()), -1);
    for (int t = 0; t < ns.elements(); ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        for (int i = 0; i < es.perElement(); ++i) a_edgeEq[std::size_t(es.dofs(t)[i])] = -2;
        for (int i = 0; i < ns.perElement(); ++i) a_nodeEq[std::size_t(ns.dofs(t)[i])] = -2;
    }
    std::vector<int> condition(topo.boundaries.size(), 0);   // 1 PEC, 2 PMC: the last feature's
    auto inside = [&](int d) { return d >= 0 && f.domains[std::size_t(d)]; };
    a_sheets.clear();
    a_sheetOf.assign(topo.boundaries.size(), -1);
    for (const Node& feature : a_node->children) {
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k || k->selection != Level::Boundary) continue;
        const QVector<int> picked = resolveSelection(topo, a_s.model(), Level::Boundary, feature.selection(), &why);
        if (feature.type.endsWith(QLatin1String("/sheet"))) {
            Sheet sh;
            sh.name = feature.name();
            sh.graphene = feature.text(QStringLiteral("model")) != QLatin1String("user");
            if (sh.graphene) {
                sh.muc = compile(feature.text(QStringLiteral("muc")), feature, QStringLiteral("muc"));
                sh.tau = compile(feature.text(QStringLiteral("tau")), feature, QStringLiteral("tau"));
                sh.T = compile(feature.text(QStringLiteral("T")), feature, QStringLiteral("T"));
            } else {
                sh.re = compile(feature.text(QStringLiteral("sigmas")), feature, QStringLiteral("sigmas"));
                sh.im = compile(feature.text(QStringLiteral("sigmas_im")), feature, QStringLiteral("sigmas_im"));
            }
            for (int b : picked) a_sheetOf[std::size_t(b)] = int(a_sheets.size());
            a_sheets.push_back(sh);
            continue;
        }
        const int c = feature.type.endsWith(QLatin1String("/pec")) ? 1 : feature.type.endsWith(QLatin1String("/pmc")) ? 2 : 0;
        if (!c) continue;
        for (int b : picked) {
            const Topology::Boundary& bd = topo.boundaries[std::size_t(b)];
            // The default conductor on the outside only; one chosen anywhere.
            if (feature.isDefault && inside(bd.up) == inside(bd.down)) continue;
            condition[std::size_t(b)] = c;
        }
    }
    // On a conductor: Et's edges and Ez's nodes (an edge's middle too).
    for (int e = 0; e < int(mesh.edges.size()); ++e) {
        const Mesh::Edge& me = mesh.edges[std::size_t(e)];
        if (condition[std::size_t(me.boundary)] != 1) continue;
        for (int dof : es.edgeDofs(me.a, me.b))
            if (dof >= 0 && a_edgeEq[std::size_t(dof)] != -1) a_edgeEq[std::size_t(dof)] = -3;
        for (int dof : ns.edgeDofs(e))
            if (dof >= 0 && a_nodeEq[std::size_t(dof)] != -1) a_nodeEq[std::size_t(dof)] = -3;
    }
    a_n = 0;
    for (int& q : a_edgeEq)
        if (q == -2) q = a_n++;
    a_nEdge = a_n;
    for (int& q : a_nodeEq)
        if (q == -2) q = a_n++;
    if (a_n == 0) {
        *error = tr("%1: nothing to solve - every field is fixed").arg(a_node->name());
        return false;
    }
    return true;
}

int ModeProblem::solveModes(const SolveOptions& o, int count, double guess, QString* error)
{
    Field& f = field();
    const EdgeSpace& es = *f.edges;
    const Space& ns = *f.space;
    const Mesh& mesh = a_s.mesh();
    const double omega = 2 * M_PI * o.frequency;
    a_k0 = omega / C0;
    const double k0 = a_k0;
    for (const QString& w : a_media.outOfRange(omega)) a_s.log << w;
    if (!(k0 > 0)) {
        *error = tr("%1: a mode analysis wants a frequency more than 0").arg(a_node->name());
        return -1;
    }
    if (!(guess > 0)) guess = a_media.largestIndex(a_s, f.domains, omega);
    const cd sigma = -(k0 * guess) * (k0 * guess);
    // A and B, element by element (A x = -β² B x for x = (β Et, -j Ez)); and
    // the coupling of Et to Ez an ε's xz, yz, zx, zy make (C, of -k0² ε_tz,
    // -k0² ε_zt).
    std::vector<Eigen::Triplet<cd>> ta, tb, tc;
    const int pe = es.perElement(), pn = ns.perElement();
    const int degree = ns.order() == 1 ? 2 : 4;
    const TriangleRule& rule = triangleRule(degree);
    PointEvaluator ev(a_s);
    std::vector<cd> Ae(std::size_t(pe * pe)), Be(std::size_t((pe + pn) * (pe + pn))), Ce(std::size_t((pe + pn) * (pe + pn)));
    bool coupled = false;
    for (int t = 0; t < ns.elements(); ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        const AffineMap am = affineMap(ns, t);
        std::fill(Ae.begin(), Ae.end(), 0);
        std::fill(Be.begin(), Be.end(), 0);
        std::fill(Ce.begin(), Ce.end(), 0);
        bool here = false;
        EdgeSpace::Basis eb;
        Shape sh;
        for (const auto& q : rule.points) {
            ev.moveTo(t, q[0], q[1]);
            const Media::At m = a_media.at(ev, d, omega, k0);
            const double w = q[2] * std::abs(am.det);
            es.basis(t, am, q[0], q[1], eb);
            shapeAt(ns.order(), q[0], q[1], sh);
            double lx[6], ly[6];
            const double i00 = am.grad[1][0], i01 = am.grad[2][0], i10 = am.grad[1][1], i11 = am.grad[2][1];
            for (int j = 0; j < pn; ++j) {
                // ∇L from the reference gradient: ∇ = (dxi, deta) through ∇λ1, ∇λ2.
                lx[j] = sh.dxi[j] * i00 + sh.deta[j] * i01;
                ly[j] = sh.dxi[j] * i10 + sh.deta[j] * i11;
            }
            const cd ix = 1.0 / m.mu[1], iy = 1.0 / m.mu[0], iz = 1.0 / m.mu[2];   // (x parts with 1/µyy, y parts 1/µxx)
            const cd (&eps)[3][3] = m.epsT;
            for (int i = 0; i < pe; ++i)
                for (int j = 0; j < pe; ++j) {
                    // w_i · ε_tt w_j
                    const cd ee = eb.vx[i] * (eps[0][0] * eb.vx[j] + eps[0][1] * eb.vy[j]) + eb.vy[i] * (eps[1][0] * eb.vx[j] + eps[1][1] * eb.vy[j]);
                    Ae[std::size_t(i * pe + j)] += w * (eb.curl[i] * eb.curl[j] * iz - k0 * k0 * ee);
                    Be[std::size_t(i * (pe + pn) + j)] += w * (eb.vx[i] * eb.vx[j] * ix + eb.vy[i] * eb.vy[j] * iy);
                }
            for (int i = 0; i < pe; ++i)
                for (int j = 0; j < pn; ++j) {
                    const cd g = w * (eb.vx[i] * lx[j] * ix + eb.vy[i] * ly[j] * iy);
                    Be[std::size_t(i * (pe + pn) + pe + j)] += g;
                    Be[std::size_t((pe + j) * (pe + pn) + i)] += g;
                }
            for (int i = 0; i < pn; ++i)
                for (int j = 0; j < pn; ++j)
                    Be[std::size_t((pe + i) * (pe + pn) + pe + j)] +=
                        w * (lx[i] * lx[j] * ix + ly[i] * ly[j] * iy - k0 * k0 * eps[2][2] * sh.n[i] * sh.n[j]);
            if (m.coupled) {
                here = true;
                for (int i = 0; i < pe; ++i)
                    for (int j = 0; j < pn; ++j) {
                        Ce[std::size_t(i * (pe + pn) + pe + j)] += -k0 * k0 * w * (eb.vx[i] * eps[0][2] + eb.vy[i] * eps[1][2]) * sh.n[j];
                        Ce[std::size_t((pe + j) * (pe + pn) + i)] += -k0 * k0 * w * sh.n[j] * (eps[2][0] * eb.vx[i] + eps[2][1] * eb.vy[i]);
                    }
            }
        }
        int eq[14];
        for (int i = 0; i < pe; ++i) eq[i] = a_edgeEq[std::size_t(es.dofs(t)[i])];
        for (int i = 0; i < pn; ++i) eq[pe + i] = a_nodeEq[std::size_t(ns.dofs(t)[i])];
        for (int i = 0; i < pe; ++i)
            for (int j = 0; j < pe; ++j)
                if (eq[i] >= 0 && eq[j] >= 0) ta.emplace_back(eq[i], eq[j], Ae[std::size_t(i * pe + j)]);
        for (int i = 0; i < pe + pn; ++i)
            for (int j = 0; j < pe + pn; ++j) {
                if (eq[i] < 0 || eq[j] < 0) continue;
                if (Be[std::size_t(i * (pe + pn) + j)] != cd(0)) tb.emplace_back(eq[i], eq[j], Be[std::size_t(i * (pe + pn) + j)]);
                if (here && Ce[std::size_t(i * (pe + pn) + j)] != cd(0)) tc.emplace_back(eq[i], eq[j], Ce[std::size_t(i * (pe + pn) + j)]);
            }
        coupled = coupled || here;
    }
    // Conductive sheets: -k0² ∮ εs (w_i·t)(w_j·t) into A, -k0² ∮ εs N_i N_j
    // into B's zz part, εs = -j σs/(ω ε0) (a layer's ε times its thickness).
    for (int e = 0; e < int(mesh.edges.size()); ++e) {
        const Mesh::Edge& me = mesh.edges[std::size_t(e)];
        const int sheet = me.boundary >= 0 && me.boundary < int(a_sheetOf.size()) ? a_sheetOf[std::size_t(me.boundary)] : -1;
        if (sheet < 0) continue;
        int t = -1;
        for (int side : {me.left, me.right})
            if (side >= 0 && f.domains[std::size_t(mesh.domain[std::size_t(side)])]) {
                t = side;
                break;
            }
        if (t < 0) continue;
        const AffineMap am = affineMap(ns, t);
        const std::array<int, 3>& tv = mesh.triangles[std::size_t(t)];
        const int k = Space::localEdge(tv, me.a, me.b);
        const QPointF pa = mesh.nodes[std::size_t(me.a)] * mesh.unitScale, pb = mesh.nodes[std::size_t(me.b)] * mesh.unitScale;
        const double len = std::hypot(pb.x() - pa.x(), pb.y() - pa.y());
        if (!(len > 0)) continue;
        const double tx = (pb.x() - pa.x()) / len, ty = (pb.y() - pa.y()) / len;
        int eq[14];
        for (int i = 0; i < pe; ++i) eq[i] = a_edgeEq[std::size_t(es.dofs(t)[i])];
        for (int i = 0; i < pn; ++i) eq[pe + i] = a_nodeEq[std::size_t(ns.dofs(t)[i])];
        for (const auto& q : lineRule(ns.order() + 3)) {
            // (The edge's point from a, in the triangle's reference.)
            const double r = tv[std::size_t(k)] == me.a ? q[0] : 1 - q[0];
            const double xi = k == 0 ? r : k == 1 ? 1 - r : 0, eta = k == 0 ? 0 : k == 1 ? r : 1 - r;
            ev.moveTo(t, xi, eta);
            const cd epsS = -J * sheetConductivity(ev, a_sheets[std::size_t(sheet)], omega) / (omega * Eps0);
            EdgeSpace::Basis eb;
            Shape sh;
            es.basis(t, am, xi, eta, eb);
            shapeAt(ns.order(), xi, eta, sh);
            const double w = q[1] * len;
            for (int i = 0; i < pe; ++i)
                for (int j = 0; j < pe; ++j)
                    if (eq[i] >= 0 && eq[j] >= 0)
                        ta.emplace_back(eq[i], eq[j], -k0 * k0 * w * epsS * (eb.vx[i] * tx + eb.vy[i] * ty) * (eb.vx[j] * tx + eb.vy[j] * ty));
            for (int i = 0; i < pn; ++i)
                for (int j = 0; j < pn; ++j)
                    if (eq[pe + i] >= 0 && eq[pe + j] >= 0) tb.emplace_back(eq[pe + i], eq[pe + j], -k0 * k0 * w * epsS * sh.n[i] * sh.n[j]);
        }
    }
    Eigen::SparseMatrix<cd> A(a_n, a_n), B(a_n, a_n);
    A.setFromTriplets(ta.begin(), ta.end());
    B.setFromTriplets(tb.begin(), tb.end());
    // Coupled: (K0 + β K1 + β² K2) x = 0 for x = (β Et, -j Ez), as the β²
    // form's - K0 = A (its transverse part), K1 = j C_tz - j C_zt (ε's tz and
    // zt), K2 = B: the β² form's A x = -β² B x with the coupling between,
    // free of spurious modes as it is (its extra eigenvalues at β = 0, none
    // at infinity: B is regular). Found by a shift and invert of its
    // linearization about β = k0 n (the forward modes).
    const cd shift = k0 * guess;
    Eigen::SparseMatrix<cd> K0(a_n, a_n), K1(a_n, a_n), K2(a_n, a_n);
    Eigen::SparseLU<Eigen::SparseMatrix<cd>> lu;
    if (coupled) {
        std::vector<Eigen::Triplet<cd>> t1;
        for (const auto& x : tc) t1.emplace_back(x.row(), x.col(), (x.row() < a_nEdge ? J : -J) * x.value());
        K0 = A;
        K1.setFromTriplets(t1.begin(), t1.end());
        K2 = B;
        // (In t = β/σ, the linearization's halves alike in size: K1 σ, K2 σ².)
        K1 *= shift;
        K2 *= shift * shift;
        Eigen::SparseMatrix<cd> P = K0 + K1 + K2;
        P.makeCompressed();
        lu.analyzePattern(P);
        lu.factorize(P);
        a_s.log << tr("%1: its media couple Ez to the transverse field (a tensor's xz, yz): its modes found by β, the forward ones").arg(a_node->name());
    } else {
        Eigen::SparseMatrix<cd> M = A - sigma * B;
        M.makeCompressed();
        lu.analyzePattern(M);
        lu.factorize(M);
    }
    if (lu.info() != Eigen::Success) {
        *error = tr("%1: the shifted matrix is singular - %2 (another effective index to search about)")
                     .arg(a_node->name(), QString::fromStdString(lu.lastErrorMessage()));
        return -1;
    }
    // With absorbing layers, more than asked: the layers' own modes (their
    // fields in the layers mostly) are left out.
    bool layers = false;
    for (std::size_t d = 0; d < a_media.domains.size(); ++d) layers = layers || (f.domains[d] && a_media.domains[d].pml);
    const int wanted = std::max(1, count);
    // The share of a mode's |E|² in the layers.
    // A mode's share of |E|² in the domains \a in picks.
    auto shareIn = [&](const Eigen::VectorXcd& x, cd beta, const std::function<bool(int)>& in) {
        double all = 0, there = 0;
        EdgeSpace::Basis eb;
        Shape sh;
        const TriangleRule& q2 = triangleRule(2);
        for (int t = 0; t < ns.elements(); ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            const AffineMap am = affineMap(ns, t);
            double e2 = 0;
            for (const auto& q : q2.points) {
                es.basis(t, am, q[0], q[1], eb);
                shapeAt(ns.order(), q[0], q[1], sh);
                cd ex = 0, ey = 0, ez = 0;
                for (int i = 0; i < pe; ++i) {
                    const int k = a_edgeEq[std::size_t(es.dofs(t)[i])];
                    if (k < 0) continue;
                    ex += x(k) * eb.vx[i];
                    ey += x(k) * eb.vy[i];
                }
                for (int i = 0; i < pn; ++i) {
                    const int k = a_nodeEq[std::size_t(ns.dofs(t)[i])];
                    if (k >= 0) ez += x(k) * sh.n[i];
                }
                e2 += q[2] * std::abs(am.det) * ((std::norm(ex) + std::norm(ey)) / std::norm(beta) + std::norm(ez));
            }
            all += e2;
            if (in(d)) there += e2;
        }
        return all > 0 ? there / all : 0.0;
    };
    auto inLayers = [&](const Eigen::VectorXcd& x, cd beta) { return shareIn(x, beta, [&](int d) { return a_media.domains[std::size_t(d)].pml; }); };
    // A mode's TE fraction: of its transverse |E|² (outside the absorbing
    // layers), the part along x - 1 a TE mode of layers along y, 0 a TM one.
    auto teFraction = [&](const Eigen::VectorXcd& x) {
        double along = 0, all = 0;
        EdgeSpace::Basis eb;
        const TriangleRule& q2 = triangleRule(2);
        for (int t = 0; t < ns.elements(); ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (d < 0 || !f.domains[std::size_t(d)] || a_media.domains[std::size_t(d)].pml) continue;
            const AffineMap am = affineMap(ns, t);
            for (const auto& q : q2.points) {
                es.basis(t, am, q[0], q[1], eb);
                cd ex = 0, ey = 0;
                for (int i = 0; i < pe; ++i) {
                    const int k = a_edgeEq[std::size_t(es.dofs(t)[i])];
                    if (k < 0) continue;
                    ex += x(k) * eb.vx[i];
                    ey += x(k) * eb.vy[i];
                }
                along += q[2] * std::abs(am.det) * std::norm(ex);
                all += q[2] * std::abs(am.det) * (std::norm(ex) + std::norm(ey));
            }
        }
        return all > 0 ? along / all : NaN;
    };
    // Tracking domains: more modes found, those most in them kept.
    a_tracking = false;
    for (char c : o.track) a_tracking = a_tracking || c;
    auto tracked = [&](int d) { return std::size_t(d) < o.track.size() && o.track[std::size_t(d)]; };
    // (As many again while too few are left, the layers' crowding the guess
    // - or the other polarization's, or those below the index or above the
    // loss asked.)
    const bool polarized = o.polarization == 1 || o.polarization == 2;
    const bool filtered = o.minNeff > 0 || std::isfinite(o.maxAlpha);
    int dropped = 0, crossed = 0, unguided = 0, products = 0, restarts = 0, keptBefore = -1;
    for (int asked = a_tracking ? std::max(2 * wanted + 4, wanted + 6) : layers || polarized || filtered ? 2 * wanted + 4 : wanted;; asked *= 2) {
        asked = std::min(asked, std::max(1, a_n - 2));
        const EigenResult r = coupled ? largestEigenvalues(2 * a_n, asked,
                                                           [&](const Eigen::VectorXcd& v, Eigen::VectorXcd& y) {
                                                               // z1 = -P⁻¹ (K1 x1 + K2 x2 + σ K2 x1), z2 = x1 + σ z1.
                                                               const Eigen::VectorXcd x1 = v.head(a_n), x2 = v.tail(a_n);
                                                               const Eigen::VectorXcd z1 = -lu.solve(K1 * x1 + K2 * (x2 + x1));
                                                               y.resize(2 * a_n);
                                                               y.head(a_n) = z1;
                                                               y.tail(a_n) = x1 + z1;
                                                           })
                                      : largestEigenvalues(a_n, asked, [&](const Eigen::VectorXcd& x, Eigen::VectorXcd& y) { y = lu.solve(B * x); });
        if (!r.error.isEmpty()) a_s.log << tr("Warning: %1: %2").arg(a_node->name(), r.error);
        products += r.products;
        restarts += r.restarts;
        a_modes.clear();
        dropped = 0;
        crossed = 0;
        unguided = 0;
        for (std::size_t i = 0; i < r.values.size(); ++i) {
            cd beta;
            Eigen::VectorXcd x;
            if (coupled) {
                beta = shift * (1.0 + 1.0 / r.values[i]);
                // (K0's null space - Ez alone, β nought - is no mode.)
                if (std::abs(beta) < 1e-4 * k0) continue;
                x = r.vectors[i].head(a_n);
            } else {
                const cd lambda = sigma + 1.0 / r.values[i];
                // (The null space of A - its Ez alone, λ nought - is no mode.)
                if (std::abs(lambda) < 1e-8 * k0 * k0) continue;
                beta = std::sqrt(-lambda);
                if (beta.real() < 0) beta = -beta;
                x = r.vectors[i];
            }
            if (layers && inLayers(x, beta) > 0.5) {
                ++dropped;
                continue;
            }
            // Its phase, which the eigensolver leaves to chance: its largest
            // transverse value real and more than nought.
            Eigen::Index largest = 0;
            if (a_nEdge > 0) x.head(a_nEdge).cwiseAbs().maxCoeff(&largest);
            if (std::abs(x(largest)) > 0) x *= std::conj(x(largest)) / std::abs(x(largest));
            const double te = teFraction(x);
            if (polarized && (o.polarization == 1 ? !(te >= o.teFraction) : !(te < 1 - o.teFraction))) {
                ++crossed;
                continue;
            }
            // Guided: above the index asked, its power's loss below that asked.
            if (!(beta.real() / k0 > o.minNeff) || !(-2 * beta.imag() < o.maxAlpha)) {
                ++unguided;
                continue;
            }
            a_modes.push_back({beta, x, a_tracking ? shareIn(x, beta, tracked) : NaN, te});
        }
        if ((!layers && !polarized && !filtered) || int(a_modes.size()) >= wanted || asked >= 96 || asked >= a_n - 2) break;
        // (Guided ones asked: twice as many found and none more kept - there
        // are no more about the index.)
        if (filtered && int(a_modes.size()) == keptBefore) break;
        keptBefore = int(a_modes.size());
    }
    // By their effective index, the highest first; those nearest the guess
    // as many as asked - or, tracking, those most in the domains tracked.
    if (int(a_modes.size()) > wanted) {
        std::sort(a_modes.begin(), a_modes.end(), [&](const Mode& a, const Mode& b) {
            if (a_tracking) return a.share > b.share;
            return coupled ? std::abs(a.beta - shift) < std::abs(b.beta - shift) : std::abs(a.beta * a.beta + sigma) < std::abs(b.beta * b.beta + sigma);
        });
        a_modes.resize(std::size_t(wanted));
    }
    if (a_tracking) {
        QStringList shares;
        for (const Mode& m : a_modes) shares << formatNumber(m.share, 3);
        a_s.log << tr("%1: the modes most in the domains tracked: their shares of |E|² %2").arg(a_node->name(), shares.join(QLatin1String(", ")));
    }
    std::sort(a_modes.begin(), a_modes.end(), [](const Mode& a, const Mode& b) { return a.beta.real() > b.beta.real(); });
    // The one chosen: the first, or the most in the domains tracked, the
    // most TE, the most TM.
    a_chosen = 0;
    for (int k = 1; k < int(a_modes.size()); ++k) {
        const Mode &m = a_modes[std::size_t(k)], &best = a_modes[std::size_t(a_chosen)];
        if ((o.choose == 1 && m.share > best.share) || (o.choose == 2 && m.te > best.te) || (o.choose == 3 && m.te < best.te)) a_chosen = k;
    }
    if (dropped) a_s.log << tr("%1: %2 modes of the absorbing layers left out").arg(a_node->name()).arg(dropped);
    if (crossed)
        a_s.log << tr("%1: %2 %3 modes left out").arg(a_node->name()).arg(crossed).arg(o.polarization == 1 ? tr("TM") : tr("TE"));
    if (unguided)
        a_s.log << tr("%1: %2 modes below the effective index or above the loss asked left out").arg(a_node->name()).arg(unguided);
    if (o.choose != 0 && !a_modes.empty()) a_s.log << tr("%1: mode %2 chosen").arg(a_node->name()).arg(a_chosen + 1);
    a_s.log << tr("%1: %2 modes at %3 about n = %4 (%5 products, %6 restarts)")
                   .arg(a_node->name())
                   .arg(a_modes.size())
                   .arg(formatQuantity(o.frequency, Dim::of(0, 0, -1)), formatNumber(guess, 5))
                   .arg(products)
                   .arg(restarts);
    f.solved = true;
    return int(a_modes.size());
}

void ModeProblem::selectMode(int k)
{
    Field& f = field();
    if (k < 0 || k >= int(a_modes.size())) return;
    a_selected = k;
    const Mode& md = a_modes[std::size_t(k)];
    const int ne = f.edges->size(), nn = f.space->size();
    f.values.assign(std::size_t(2 * ne + 2 * nn), 0.0);
    // Et = et / β, Ez = j ez.
    for (int dof = 0; dof < ne; ++dof)
        if (a_edgeEq[std::size_t(dof)] >= 0) {
            const cd v = md.x(a_edgeEq[std::size_t(dof)]) / md.beta;
            f.values[std::size_t(dof)] = v.real();
            f.values[std::size_t(ne + dof)] = v.imag();
        }
    for (int dof = 0; dof < nn; ++dof)
        if (a_nodeEq[std::size_t(dof)] >= 0) {
            const cd v = J * md.x(a_nodeEq[std::size_t(dof)]);
            f.values[std::size_t(2 * ne + dof)] = v.real();
            f.values[std::size_t(2 * ne + nn + dof)] = v.imag();
        }
    // extra: β, k0, then by triangle what it absorbs (MediumAt's layout:
    // at its middle - n, κ across the gap, the direct gap's κ and its value
    // without a field, -Im ε along x, y, z).
    const int nt = f.space->elements();
    f.extra.assign(std::size_t(3 + MediumAt::PerTriangle * nt), NaN);
    f.extra[0] = md.beta.real();
    f.extra[1] = md.beta.imag();
    f.extra[2] = a_k0;
    const double omega = a_k0 * C0;
    const Mesh& mesh = a_s.mesh();
    PointEvaluator ev(a_s);
    for (int t = 0; t < nt; ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)] || a_media.domains[std::size_t(d)].pml) continue;
        ev.moveTo(t, 1.0 / 3, 1.0 / 3);
        const Media::At m = a_media.at(ev, d, omega, a_k0);
        if (!m.ok) continue;
        double* e = &f.extra[std::size_t(3 + MediumAt::PerTriangle * t)];
        e[0] = m.n;
        e[1] = m.kappaGap;
        e[2] = m.kappaEA;
        e[3] = m.kappaEA0;
        e[13] = m.dnCarriers;
        e[14] = m.alphaCarriers;
        for (int pol = 0; pol < 2; ++pol) {
            e[15 + pol] = m.dnWells[pol];
            e[17 + pol] = m.alphaWells[pol];
        }
        // ε'' = (εᴴ - ε)/2j: its diagonal, then xy, xz, yz (re, im).
        auto loss = [&m](int i, int j) { return (std::conj(m.epsT[j][i]) - m.epsT[i][j]) / (2.0 * J); };
        for (int c = 0; c < 3; ++c) e[4 + c] = loss(c, c).real();
        const int pairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};
        for (int k = 0; k < 3; ++k) {
            const cd v = loss(pairs[k][0], pairs[k][1]);
            e[7 + 2 * k] = v.real();
            e[8 + 2 * k] = v.imag();
        }
    }
    // Its power along z, 1 W: Sz = ½ Re(Ex Hy* - Ey Hx*) over the domains;
    // what it absorbs across the gap, ω ε0 n κ |E|² (the direct gap's, less
    // its value without a field).
    double power = 0, largest = 0, gap = 0, field = 0;
    const TriangleRule& rule = triangleRule(f.space->order() == 1 ? 2 : 4);
    for (int t = 0; t < nt; ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)] || a_media.domains[std::size_t(d)].pml) continue;
        const AffineMap am = affineMap(*f.space, t);
        const double* e = &f.extra[std::size_t(3 + MediumAt::PerTriangle * t)];
        for (const auto& q : rule.points) {
            ev.moveTo(t, q[0], q[1]);
            const ModeFields mf = modeFieldsAt(ev, a_fi);
            if (!mf.defined) continue;
            const double w = q[2] * std::abs(am.det);
            power += w * 0.5 * (mf.E[0] * std::conj(mf.H[1]) - mf.E[1] * std::conj(mf.H[0])).real();
            const double e2 = std::norm(mf.E[0]) + std::norm(mf.E[1]) + std::norm(mf.E[2]);
            largest = std::max(largest, std::sqrt(e2));
            if (std::isfinite(e[0])) {
                gap += w * omega * Eps0 * e[0] * e[1] * e2;
                field += w * omega * Eps0 * e[0] * (e[2] - e[3]) * e2;
            }
        }
    }
    const double scale = power > 1e-300 ? 1 / std::sqrt(power) : largest > 0 ? 1 / largest : 1.0;
    for (double& v : f.values) v *= scale;
    a_absorbedGap = gap * scale * scale;
    a_absorbedField = field * scale * scale;
    // What its sheets take: ½ ∮ Re σs (|E·t|² + |Ez|²) ds, per metre.
    a_sheetPower = 0;
    for (int e = 0; e < int(mesh.edges.size()) && !a_sheets.empty(); ++e) {
        const Mesh::Edge& me = mesh.edges[std::size_t(e)];
        const int sheet = me.boundary >= 0 && me.boundary < int(a_sheetOf.size()) ? a_sheetOf[std::size_t(me.boundary)] : -1;
        if (sheet < 0) continue;
        int t = -1;
        for (int side : {me.left, me.right})
            if (side >= 0 && f.domains[std::size_t(mesh.domain[std::size_t(side)])]) {
                t = side;
                break;
            }
        if (t < 0) continue;
        const std::array<int, 3>& tv = mesh.triangles[std::size_t(t)];
        const int k = Space::localEdge(tv, me.a, me.b);
        const QPointF pa = mesh.nodes[std::size_t(me.a)] * mesh.unitScale, pb = mesh.nodes[std::size_t(me.b)] * mesh.unitScale;
        const double len = std::hypot(pb.x() - pa.x(), pb.y() - pa.y());
        if (!(len > 0)) continue;
        for (const auto& q : lineRule(f.space->order() + 3)) {
            const double r = tv[std::size_t(k)] == me.a ? q[0] : 1 - q[0];
            ev.moveTo(t, k == 0 ? r : k == 1 ? 1 - r : 0, k == 0 ? 0 : k == 1 ? r : 1 - r);
            const ModeFields mf = modeFieldsAt(ev, a_fi);
            if (!mf.defined) continue;
            const cd et = (mf.E[0] * (pb.x() - pa.x()) + mf.E[1] * (pb.y() - pa.y())) / len;
            a_sheetPower += 0.5 * q[1] * len * sheetConductivity(ev, a_sheets[std::size_t(sheet)], omega).real() * (std::norm(et) + std::norm(mf.E[2]));
        }
    }
    a_s.setMode(k + 1);
}

void ModeProblem::setGlobals()
{
    if (a_selected < 0 || a_selected >= int(a_modes.size())) return;
    const cd beta = a_modes[std::size_t(a_selected)].beta;
    const QString p = field().tag + QLatin1Char('.');
    const Dim perMetre = Dim::of(-1);
    a_s.setGlobal(p + QStringLiteral("neff"), beta.real() / a_k0, Dim(), tr("effective index: β/k0"));
    a_s.setGlobal(p + QStringLiteral("beta"), beta.real(), perMetre, tr("propagation constant β"));
    a_s.setGlobal(p + QStringLiteral("alpha"), -beta.imag(), perMetre, tr("attenuation constant α (Np/m)"));
    a_s.setGlobal(p + QStringLiteral("loss"), -beta.imag() * 20 / std::log(10.0), perMetre, tr("loss in dB per metre"), QStringLiteral("dB/m"));
    a_s.setGlobal(p + QStringLiteral("neff_im"), -beta.imag() / a_k0, Dim(), tr("effective index's imaginary part, its loss (β = k0 (neff - j neff_im))"));
    a_s.setGlobal(p + QStringLiteral("lambda0"), 2 * M_PI / a_k0, Dim::length(), tr("free-space wavelength"));
    a_s.setGlobal(p + QStringLiteral("mode"), a_selected + 1, Dim(), tr("the mode's number, by effective index"));
    a_s.setGlobal(p + QStringLiteral("TEfraction"), a_modes[std::size_t(a_selected)].te, Dim(),
                  tr("of the mode's transverse |E|², the part along x: near 1 a TE mode (E in the plane of layers along y), near 0 a TM "
                     "one"));
    if (a_tracking)
        a_s.setGlobal(p + QStringLiteral("Gamma"), a_modes[std::size_t(a_selected)].share, Dim(),
                      tr("the mode's share of |E|² in the domains its step tracks: a confinement factor"));
    // (Its power's, ∫ ω ε0 n κ |E|² over 1 W: a perturbation, as small losses are.)
    const double dB = 10 / std::log(10.0);
    a_s.setGlobal(p + QStringLiteral("loss_gap"), a_absorbedGap * dB, perMetre,
                  tr("loss in dB per metre absorbed across a gap - the media's κ and a direct gap's, not the free carriers': what makes "
                     "electron-hole pairs"),
                  QStringLiteral("dB/m"));
    if (!a_sheets.empty()) {
        a_s.setGlobal(p + QStringLiteral("Psheet"), a_sheetPower, Dim::of(1, 1, -3),
                      tr("power the conductive sheets absorb per metre, the mode carrying 1 W (W/m)"));
        a_s.setGlobal(p + QStringLiteral("loss_sheet"), a_sheetPower * dB, perMetre, tr("loss in dB per metre the conductive sheets absorb"),
                      QStringLiteral("dB/m"));
    }
    if (a_electroabsorption)
        a_s.setGlobal(p + QStringLiteral("dloss_EA"), a_absorbedField * dB, perMetre,
                      tr("loss in dB per metre the field adds to the direct gap's absorption (the Franz-Keldysh effect): its "
                         "electroabsorption over none at all"),
                      QStringLiteral("dB/m"));
}

// ------------------------------------------------------------------ A body of revolution's resonances

/*!
 * A body of revolution's resonant modes, of one azimuthal order m: the
 * fields E(r, z) e^{-jmφ}, full-vector - (E_r, E_z) on Nédélec's edge
 * elements of the meridian plane, χ = -j r E_φ on Lagrange's nodes. The
 * curl's r and z parts are j(m E_z + ∂z χ)/r and j(m E_r + ∂r χ)/r, its φ
 * part the plane's curl of E_t:
 *   ∫ [(m E_r + ∂r χ)²/(µ_z r) + (m E_z + ∂z χ)²/(µ_r r) + r (curl E_t)²/µ_φ]
 *     = k0² ∫ [ε_r r E_r² + ε_z r E_z² + ε_φ χ²/r]
 * - symmetric; complex with lossy media and the layers' (cylindrical)
 * stretching. On the axis χ = 0, and E_z = 0 for m ≠ 0. Its eigenvalues
 * k0² about the one searched for, shifted and inverted: ω = c k0 complex,
 * a mode the layers or its media take decaying, Q = Re ω / 2 Im ω.
 */
class ResonatorProblem : public Problem
{
public:
    ResonatorProblem(Solution& solution, int field) : Problem(solution, field) {}

    bool setup(QString* error) override;
    void initialize() override
    {
        Field& f = field();
        f.values.assign(std::size_t(2 * f.edges->size() + 2 * f.space->size()), 0.0);
        f.extra.assign(3, 0.0);
    }
    bool solve(const SolveOptions&, QString* error) override
    {
        *error = tr("%1: a resonator's modes are solved by an Eigenfrequency step").arg(a_node->name());
        return false;
    }
    bool modal() const override { return true; }
    int solveModes(const SolveOptions& options, int count, double guess, QString* error) override;
    void selectMode(int k) override;
    void setGlobals() override;

private:
    Media a_media;
    std::vector<int> a_edgeEq, a_nodeEq;   // equations, -1 none (fixed or not here)
    int a_n = 0, a_m = 0;
    struct Mode {
        cd k;                 // k0 = ω/c, complex
        Eigen::VectorXcd x;   // E_t then χ, by equation
    };
    std::vector<Mode> a_modes;
    int a_selected = -1;
    double a_target = 0;      // the k0 searched about
};

bool ResonatorProblem::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    if (!a_s.axisymmetric()) {
        *error = tr("%1: a resonator here is a body of revolution's - its component's space axisymmetric (the axis x = 0, r along "
                    "x, z along y)")
                     .arg(a_node->name());
        return false;
    }
    Field& f = field();
    const Topology& topo = a_s.topology();
    QString why;
    const QVector<int> domains = resolveSelection(topo, a_s.model(), Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(topo.domains.size(), 0);
    for (int d : domains) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    a_media.cylindrical = true;
    a_media.setup(a_s, *a_node, f.domains, materials(), mediaHooks());
    a_m = a_node->integer(QStringLiteral("m"));
    Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    coef.c.assign(topo.domains.size(), Expression());
    std::vector<Expression> mur(topo.domains.size());
    for (std::size_t d = 0; d < a_media.domains.size(); ++d) {
        if (a_media.domains[d].defined && a_media.domains[d].byIndex) coef.c[d] = a_media.domains[d].n[0];
        mur[d] = a_media.domains[d].mur[0];
    }
    coef.named[QStringLiteral("mur")] = mur;
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    const EdgeSpace& es = *f.edges;
    const Space& ns = *f.space;
    const Mesh& mesh = a_s.mesh();
    a_edgeEq.assign(std::size_t(es.size()), -1);
    a_nodeEq.assign(std::size_t(ns.size()), -1);
    for (int t = 0; t < ns.elements(); ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        for (int i = 0; i < es.perElement(); ++i) a_edgeEq[std::size_t(es.dofs(t)[i])] = -2;
        for (int i = 0; i < ns.perElement(); ++i) a_nodeEq[std::size_t(ns.dofs(t)[i])] = -2;
    }
    // The axis (r = 0): no boundary - χ = 0 there, and E_z = 0 for m ≠ 0;
    // a conductor elsewhere (the outside by default): E_t's edges and χ.
    const double tol = 1e-9 * std::max(1e-300, std::max(topo.bounds.width(), topo.bounds.height()));
    auto onAxis = [&](int node) { return std::abs(mesh.nodes[std::size_t(node)].x()) <= tol; };
    std::vector<int> condition(topo.boundaries.size(), 0);   // 1 PEC, 2 PMC: the last feature's
    auto inside = [&](int d) { return d >= 0 && f.domains[std::size_t(d)]; };
    for (const Node& feature : a_node->children) {
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k || k->selection != Level::Boundary) continue;
        const int c = feature.type.endsWith(QLatin1String("/pec")) ? 1 : feature.type.endsWith(QLatin1String("/pmc")) ? 2 : 0;
        if (!c) continue;
        const QVector<int> picked = resolveSelection(topo, a_s.model(), Level::Boundary, feature.selection(), &why);
        for (int b : picked) {
            const Topology::Boundary& bd = topo.boundaries[std::size_t(b)];
            if (feature.isDefault && inside(bd.up) == inside(bd.down)) continue;
            condition[std::size_t(b)] = c;
        }
    }
    int axisEdges = 0;
    for (int e = 0; e < int(mesh.edges.size()); ++e) {
        const Mesh::Edge& me = mesh.edges[std::size_t(e)];
        const bool axis = onAxis(me.a) && onAxis(me.b);
        if (axis) {
            ++axisEdges;
            if (a_m != 0)
                for (int dof : es.edgeDofs(me.a, me.b))
                    if (dof >= 0 && a_edgeEq[std::size_t(dof)] != -1) a_edgeEq[std::size_t(dof)] = -3;
            for (int dof : ns.edgeDofs(e))
                if (dof >= 0 && a_nodeEq[std::size_t(dof)] != -1) a_nodeEq[std::size_t(dof)] = -3;
            continue;
        }
        if (me.boundary < 0 || condition[std::size_t(me.boundary)] != 1) continue;
        for (int dof : es.edgeDofs(me.a, me.b))
            if (dof >= 0 && a_edgeEq[std::size_t(dof)] != -1) a_edgeEq[std::size_t(dof)] = -3;
        for (int dof : ns.edgeDofs(e))
            if (dof >= 0 && a_nodeEq[std::size_t(dof)] != -1) a_nodeEq[std::size_t(dof)] = -3;
    }
    // (A node on the axis alone - a domain touching it at a point - too.)
    for (int t = 0; t < ns.elements(); ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        for (int i = 0; i < 3; ++i) {
            const int v = mesh.triangles[std::size_t(t)][std::size_t(i)];
            if (onAxis(v)) {
                const int dof = ns.dofs(t)[i];
                if (a_nodeEq[std::size_t(dof)] != -1) a_nodeEq[std::size_t(dof)] = -3;
            }
        }
    }
    (void)axisEdges;
    for (const auto& node : mesh.nodes)
        if (node.x() < -tol) {
            *error = tr("%1: the geometry reaches x < 0 - a body of revolution's lies at r = x ≥ 0").arg(a_node->name());
            return false;
        }
    a_n = 0;
    for (int& q : a_edgeEq)
        if (q == -2) q = a_n++;
    for (int& q : a_nodeEq)
        if (q == -2) q = a_n++;
    if (a_n == 0) {
        *error = tr("%1: nothing to solve - every field is fixed").arg(a_node->name());
        return false;
    }
    return true;
}

int ResonatorProblem::solveModes(const SolveOptions& o, int count, double, QString* error)
{
    Field& f = field();
    const EdgeSpace& es = *f.edges;
    const Space& ns = *f.space;
    const Mesh& mesh = a_s.mesh();
    const double omega = 2 * M_PI * o.frequency;
    a_target = omega / C0;
    const double k0 = a_target;
    if (!(k0 > 0)) {
        *error = tr("%1: an eigenfrequency step wants a frequency or wavelength to search about").arg(a_node->name());
        return -1;
    }
    for (const QString& w : a_media.outOfRange(omega)) a_s.log << w;
    std::vector<Eigen::Triplet<cd>> tk, tm;
    const int pe = es.perElement(), pn = ns.perElement(), pa = pe + pn;
    const TriangleRule& rule = triangleRule(5);
    PointEvaluator ev(a_s);
    std::vector<cd> Ke(std::size_t(pa * pa)), Me(std::size_t(pa * pa));
    const std::size_t all = static_cast<std::size_t>(pa);
    std::vector<double> gr(all), gz(all), cu(all), er(all), ez(all), ch(all);
    const double m = a_m;
    bool warnedTensor = false;
    for (int t = 0; t < ns.elements(); ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        const AffineMap am = affineMap(ns, t);
        std::fill(Ke.begin(), Ke.end(), 0);
        std::fill(Me.begin(), Me.end(), 0);
        EdgeSpace::Basis eb;
        Shape sh;
        const std::array<int, 3>& tv = mesh.triangles[std::size_t(t)];
        const double x0 = mesh.nodes[std::size_t(tv[0])].x() * mesh.unitScale, x1 = mesh.nodes[std::size_t(tv[1])].x() * mesh.unitScale,
                     x2 = mesh.nodes[std::size_t(tv[2])].x() * mesh.unitScale;
        for (const auto& q : rule.points) {
            ev.moveTo(t, q[0], q[1]);
            // (r of the triangle the basis is of - straight: a curved one's
            // point may come to the axis where the basis's does not.)
            const double r = (1 - q[0] - q[1]) * x0 + q[0] * x1 + q[1] * x2;
            if (!(r > 0)) continue;
            const Media::At md = a_media.at(ev, d, omega, k0);
            if (md.coupled && !warnedTensor) {
                warnedTensor = true;
                a_s.log << tr("Warning: %1: a medium's tensor couples r, z and φ - its diagonal taken (a body of revolution's medium is "
                              "uniaxial along z at most)")
                               .arg(a_node->name());
            }
            const double w = q[2] * std::abs(am.det);
            es.basis(t, am, q[0], q[1], eb);
            shapeAt(ns.order(), q[0], q[1], sh);
            const double i00 = am.grad[1][0], i01 = am.grad[2][0], i10 = am.grad[1][1], i11 = am.grad[2][1];
            for (int i = 0; i < pe; ++i) {
                gr[std::size_t(i)] = m * eb.vx[i];
                gz[std::size_t(i)] = m * eb.vy[i];
                cu[std::size_t(i)] = eb.curl[i];
                er[std::size_t(i)] = eb.vx[i];
                ez[std::size_t(i)] = eb.vy[i];
                ch[std::size_t(i)] = 0;
            }
            for (int j = 0; j < pn; ++j) {
                gr[std::size_t(pe + j)] = sh.dxi[j] * i00 + sh.deta[j] * i01;
                gz[std::size_t(pe + j)] = sh.dxi[j] * i10 + sh.deta[j] * i11;
                cu[std::size_t(pe + j)] = 0;
                er[std::size_t(pe + j)] = 0;
                ez[std::size_t(pe + j)] = 0;
                ch[std::size_t(pe + j)] = sh.n[j];
            }
            const cd aR = w / (md.mu[1] * r), aZ = w / (md.mu[0] * r), aC = w * r / md.mu[2];
            const cd bR = w * md.eps[0] * r, bZ = w * md.eps[1] * r, bC = w * md.eps[2] / r;
            for (int i = 0; i < pa; ++i)
                for (int j = 0; j < pa; ++j) {
                    const std::size_t k = std::size_t(i * pa + j);
                    Ke[k] += aR * gr[std::size_t(i)] * gr[std::size_t(j)] + aZ * gz[std::size_t(i)] * gz[std::size_t(j)]
                             + aC * cu[std::size_t(i)] * cu[std::size_t(j)];
                    Me[k] += bR * er[std::size_t(i)] * er[std::size_t(j)] + bZ * ez[std::size_t(i)] * ez[std::size_t(j)]
                             + bC * ch[std::size_t(i)] * ch[std::size_t(j)];
                }
        }
        int eq[16];
        for (int i = 0; i < pe; ++i) eq[i] = a_edgeEq[std::size_t(es.dofs(t)[i])];
        for (int i = 0; i < pn; ++i) eq[pe + i] = a_nodeEq[std::size_t(ns.dofs(t)[i])];
        for (int i = 0; i < pa; ++i)
            for (int j = 0; j < pa; ++j) {
                if (eq[i] < 0 || eq[j] < 0) continue;
                const std::size_t k = std::size_t(i * pa + j);
                if (Ke[k] != cd(0)) tk.emplace_back(eq[i], eq[j], Ke[k]);
                if (Me[k] != cd(0)) tm.emplace_back(eq[i], eq[j], Me[k]);
            }
    }
    Eigen::SparseMatrix<cd> K(a_n, a_n), M(a_n, a_n);
    K.setFromTriplets(tk.begin(), tk.end());
    M.setFromTriplets(tm.begin(), tm.end());
    const cd sigma = k0 * k0;
    Eigen::SparseMatrix<cd> P = K - sigma * M;
    P.makeCompressed();
    Eigen::SparseLU<Eigen::SparseMatrix<cd>> lu;
    lu.analyzePattern(P);
    lu.factorize(P);
    if (lu.info() != Eigen::Success) {
        *error = tr("%1: the shifted matrix is singular - %2 (another frequency to search about)")
                     .arg(a_node->name(), QString::fromStdString(lu.lastErrorMessage()));
        return -1;
    }
    // The layers' own modes (their fields there mostly) and the curl's null
    // space (k0 nought) left out: more asked while too few are left.
    bool layers = false;
    for (std::size_t d = 0; d < a_media.domains.size(); ++d) layers = layers || (f.domains[d] && a_media.domains[d].pml);
    auto inLayers = [&](const Eigen::VectorXcd& x) {
        double all = 0, there = 0;
        EdgeSpace::Basis eb;
        Shape sh;
        const TriangleRule& q2 = triangleRule(2);
        for (int t = 0; t < ns.elements(); ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            const AffineMap am = affineMap(ns, t);
            double e2 = 0;
            for (const auto& q : q2.points) {
                es.basis(t, am, q[0], q[1], eb);
                shapeAt(ns.order(), q[0], q[1], sh);
                cd exr = 0, exz = 0, chi = 0;
                for (int i = 0; i < pe; ++i) {
                    const int k = a_edgeEq[std::size_t(es.dofs(t)[i])];
                    if (k < 0) continue;
                    exr += x(k) * eb.vx[i];
                    exz += x(k) * eb.vy[i];
                }
                for (int i = 0; i < pn; ++i) {
                    const int k = a_nodeEq[std::size_t(ns.dofs(t)[i])];
                    if (k >= 0) chi += x(k) * sh.n[i];
                }
                const std::array<int, 3>& tv = mesh.triangles[std::size_t(t)];
                const double r = std::max(((1 - q[0] - q[1]) * mesh.nodes[std::size_t(tv[0])].x() + q[0] * mesh.nodes[std::size_t(tv[1])].x()
                                           + q[1] * mesh.nodes[std::size_t(tv[2])].x())
                                              * mesh.unitScale,
                                          1e-300);
                e2 += q[2] * std::abs(am.det) * r * (std::norm(exr) + std::norm(exz) + std::norm(chi) / (r * r));
            }
            all += e2;
            if (a_media.domains[std::size_t(d)].pml) there += e2;
        }
        return all > 0 ? there / all : 0.0;
    };
    const int wanted = std::max(1, count);
    int products = 0, restarts = 0, dropped = 0;
    for (int asked = 2 * wanted + 4;; asked *= 2) {
        asked = std::min(asked, std::max(1, a_n - 2));
        const EigenResult r = largestEigenvalues(a_n, asked, [&](const Eigen::VectorXcd& x, Eigen::VectorXcd& y) { y = lu.solve(M * x); });
        if (!r.error.isEmpty()) a_s.log << tr("Warning: %1: %2").arg(a_node->name(), r.error);
        products += r.products;
        restarts += r.restarts;
        a_modes.clear();
        dropped = 0;
        for (std::size_t i = 0; i < r.values.size(); ++i) {
            const cd k2 = sigma + 1.0 / r.values[i];
            // (The curl's null space: gradients, k0 nought.)
            if (std::abs(k2) < 1e-6 * k0 * k0) continue;
            cd k = std::sqrt(k2);
            if (k.real() < 0) k = -k;
            if (layers && inLayers(r.vectors[i]) > 0.5) {
                ++dropped;
                continue;
            }
            a_modes.push_back({k, r.vectors[i]});
        }
        if (int(a_modes.size()) >= wanted || asked >= 96 || asked >= a_n - 2) break;
    }
    // Those nearest the frequency searched about, then by frequency.
    std::sort(a_modes.begin(), a_modes.end(), [&](const Mode& a, const Mode& b) { return std::abs(a.k - k0) < std::abs(b.k - k0); });
    if (int(a_modes.size()) > wanted) a_modes.resize(std::size_t(wanted));
    std::sort(a_modes.begin(), a_modes.end(), [](const Mode& a, const Mode& b) { return a.k.real() < b.k.real(); });
    if (dropped) a_s.log << tr("%1: %2 modes of the absorbing layers left out").arg(a_node->name()).arg(dropped);
    a_s.log << tr("%1: %2 modes of azimuthal order %3 about %4 (%5 products, %6 restarts)")
                   .arg(a_node->name())
                   .arg(a_modes.size())
                   .arg(a_m)
                   .arg(formatQuantity(o.frequency, Dim::of(0, 0, -1)))
                   .arg(products)
                   .arg(restarts);
    f.solved = true;
    return int(a_modes.size());
}

void ResonatorProblem::selectMode(int k)
{
    Field& f = field();
    if (k < 0 || k >= int(a_modes.size())) return;
    a_selected = k;
    const Mode& md = a_modes[std::size_t(k)];
    const int ne = f.edges->size(), nn = f.space->size();
    f.values.assign(std::size_t(2 * ne + 2 * nn), 0.0);
    for (int dof = 0; dof < ne; ++dof)
        if (a_edgeEq[std::size_t(dof)] >= 0) {
            const cd v = md.x(a_edgeEq[std::size_t(dof)]);
            f.values[std::size_t(dof)] = v.real();
            f.values[std::size_t(ne + dof)] = v.imag();
        }
    for (int dof = 0; dof < nn; ++dof)
        if (a_nodeEq[std::size_t(dof)] >= 0) {
            const cd v = md.x(a_nodeEq[std::size_t(dof)]);
            f.values[std::size_t(2 * ne + dof)] = v.real();
            f.values[std::size_t(2 * ne + nn + dof)] = v.imag();
        }
    // (Its extra: k0's real and imaginary parts, m, the stored energy U,
    // then each triangle's Re ε.)
    const int nt = f.space->elements();
    f.extra.assign(std::size_t(4 + nt), std::numeric_limits<double>::quiet_NaN());
    f.extra[0] = md.k.real();
    f.extra[1] = md.k.imag();
    f.extra[2] = a_m;
    f.extra[3] = 0;
    // Its largest |E| outside the layers 1 V/m, there real.
    const Mesh& mesh = a_s.mesh();
    PointEvaluator ev(a_s);
    const TriangleRule& rule = triangleRule(2);
    double largest = 0;
    cd at = 1;
    for (int t = 0; t < f.space->elements(); ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)] || a_media.domains[std::size_t(d)].pml) continue;
        for (const auto& q : rule.points) {
            ev.moveTo(t, q[0], q[1]);
            const ResonatorFields rf = resonatorFieldsAt(ev, a_fi);
            if (!rf.defined) continue;
            for (const cd& c : rf.E)
                if (std::abs(c) > largest) {
                    largest = std::abs(c);
                    at = c;
                }
        }
    }
    if (largest > 0) {
        const cd scale = std::conj(at) / (largest * largest);
        for (int dof = 0; dof < ne; ++dof) {
            const cd v = cd(f.values[std::size_t(dof)], f.values[std::size_t(ne + dof)]) * scale;
            f.values[std::size_t(dof)] = v.real();
            f.values[std::size_t(ne + dof)] = v.imag();
        }
        for (int dof = 0; dof < nn; ++dof) {
            const cd v = cd(f.values[std::size_t(2 * ne + dof)], f.values[std::size_t(2 * ne + nn + dof)]) * scale;
            f.values[std::size_t(2 * ne + dof)] = v.real();
            f.values[std::size_t(2 * ne + nn + dof)] = v.imag();
        }
    }
    // Each triangle's permittivity (the diagonal's mean, its real part) and
    // the mode's stored energy, U = ½ ε0 ∫ Re ε |E|² dV outside the layers -
    // electric and magnetic, a resonance's time average, the field as
    // normalized: what a laser's photons are counted against.
    {
        constexpr double Eps0 = 8.8541878188e-12;
        const double omega = md.k.real() * C0, k0 = md.k.real();
        const TriangleRule& q4 = triangleRule(4);
        double U = 0;
        for (int t = 0; t < nt; ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (d < 0 || !f.domains[std::size_t(d)] || a_media.domains[std::size_t(d)].pml) continue;
            ev.moveTo(t, 1.0 / 3, 1.0 / 3);
            const Media::At m = a_media.at(ev, d, omega, k0);
            const double eps = (m.eps[0].real() + m.eps[1].real() + m.eps[2].real()) / 3;
            f.extra[std::size_t(4 + t)] = eps;
            const AffineMap am = affineMap(*f.space, t);
            const std::array<int, 3>& v = mesh.triangles[std::size_t(t)];
            for (const auto& q : q4.points) {
                ev.moveTo(t, q[0], q[1]);
                const ResonatorFields rf = resonatorFieldsAt(ev, a_fi);
                if (!rf.defined) continue;
                const double r = ((1 - q[0] - q[1]) * mesh.nodes[std::size_t(v[0])].x() + q[0] * mesh.nodes[std::size_t(v[1])].x()
                                  + q[1] * mesh.nodes[std::size_t(v[2])].x())
                                 * mesh.unitScale;
                const double e2 = std::norm(rf.E[0]) + std::norm(rf.E[1]) + std::norm(rf.E[2]);
                U += q[2] * std::abs(am.det) * 2 * M_PI * r * eps * e2;
            }
        }
        f.extra[3] = 0.5 * Eps0 * U;
    }
    a_s.setMode(k + 1);
    a_s.setFrequency(md.k.real() * C0 / (2 * M_PI));
}

void ResonatorProblem::setGlobals()
{
    if (a_selected < 0 || a_selected >= int(a_modes.size())) return;
    const cd k = a_modes[std::size_t(a_selected)].k;
    const QString p = field().tag + QLatin1Char('.');
    const Dim hertz = Dim::of(0, 0, -1);
    a_s.setGlobal(p + QStringLiteral("freq"), k.real() * C0 / (2 * M_PI), hertz, tr("eigenfrequency, its real part (Hz)"));
    a_s.setGlobal(p + QStringLiteral("freq_im"), k.imag() * C0 / (2 * M_PI), hertz,
                  tr("eigenfrequency's imaginary part: the mode's decay (e^{jωt}, its energy e^{-2 Im ω t})"));
    a_s.setGlobal(p + QStringLiteral("lambda0"), 2 * M_PI / k.real(), Dim::length(), tr("free-space wavelength of the eigenfrequency, 2πc/Re ω"));
    // (Without losses Im ω is the eigensolver's round-off, 1e-16 of Re ω.)
    a_s.setGlobal(p + QStringLiteral("Q"), std::abs(k.imag()) > 1e-13 * k.real() ? k.real() / (2 * k.imag()) : std::numeric_limits<double>::infinity(),
                  Dim(), tr("quality factor: Re ω / 2 Im ω (infinite without losses)"));
    a_s.setGlobal(p + QStringLiteral("m"), a_m, Dim(), tr("azimuthal order: the fields e^{-jmφ}"));
    a_s.setGlobal(p + QStringLiteral("mode"), a_selected + 1, Dim(), tr("the mode's number, by frequency"));
    // Its stored energy and what it loses (the field as normalized).
    const Field& f = field();
    const double U = f.extra.size() > 3 ? f.extra[3] : 0;
    a_s.setGlobal(p + QStringLiteral("U"), U, Dim::of(2, 1, -2),
                  tr("stored energy, ½ ε0 ∫ Re ε |E|² dV outside the absorbing layers, of the mode as normalized (its largest |E| 1 V/m)"));
    a_s.setGlobal(p + QStringLiteral("Ploss"), 2 * k.imag() * C0 * U, Dim::of(2, 1, -3),
                  tr("power it loses at that energy, 2 Im ω U = ω U / Q (radiated, absorbed)"));
}

// ------------------------------------------------------------------ Waves in the plane

class WaveProblem : public Problem
{
public:
    WaveProblem(Solution& solution, int field) : Problem(solution, field) {}

    bool setup(QString* error) override;
    void initialize() override
    {
        Field& f = field();
        f.values.assign(std::size_t(2 * f.space->size()), 0.0);
    }
    bool solve(const SolveOptions& options, QString* error) override;
    void setGlobals() override;
    bool harmonic() const override { return true; }

private:
    struct Port {
        QString name;
        bool excite = true;
        std::vector<int> edges;     // mesh edges
        cd beta;
        Eigen::VectorXcd c;         // ∫ p e N_i ds, by equation
    };
    /// The point of triangle \a t on mesh edge \a e at s (0 at its a).
    void edgePoint(int e, double s, int* t, double* xi, double* eta) const;
    /// The plane's P (row by row: xx, xy, yx, yy) of a medium - Ez's
    /// diag(1/µyy, 1/µxx), Hz's from η = ε_t⁻¹ - and q (Ez's εzz, Hz's µzz).
    void planeCoefficients(const Media::At& md, cd P[4], cd* q) const;
    /// On a boundary of direction \a t (a unit vector along it, \a n across):
    /// P across (nᵀ P n), along (tᵀ P t), mixed (nᵀ P t), and q.
    void scalars(PointEvaluator& ev, int d, double omega, double k0, const double t[2], cd* across, cd* along, cd* mixed, cd* q) const;
    bool portMode(Port& port, double omega, double k0, QString* error);

    /// A Periodic Condition: u(r + d) = u(r) e^{-j kF·d} - each node of its
    /// destination side of the source's (interpolated where the two meshes
    /// differ).
    struct Periodic {
        QString name;
        int floquet = 1;   // 0 continuity, 1 from the fed periodic port, 2 kF given
        double kx = 0, ky = 0;
        QPointF shift;     // d, metres: from the source side to the destination
        std::vector<int> source, destination;   // boundaries
        std::vector<std::pair<int, std::vector<std::pair<int, double>>>> ties;   // equation: (equation, weight)
    };
    /// A periodic port: a unit cell's top or bottom, the plane waves of its
    /// diffraction orders e_m = e^{-j k_m·r}/√L, k_m = (kF + 2πm/L) ê.
    struct PeriodicPort {
        QString name;
        bool excite = false;
        double angle = 0;   // radians, from the inward normal toward ê
        double E0 = 1;
        std::vector<int> edges;
        QPointF along;      // ê: along it, toward +x (+y when it lies along y)
        double length = 0;  // L, metres
        cd p = 0, pt = 0, q = 0;   // its medium's: across, along it, and q
        int domain = -1;
        std::vector<int> m;
        std::vector<double> kt;   // along ê
        std::vector<cd> beta;
        std::vector<Eigen::VectorXcd> c, dual;   // ∫ N e_m ds, ∫ N ê_m ds (ê_m = e^{+j k_m·r}/√L)
        std::vector<cd> b;        // what each order carries out
        std::vector<double> power;   // of the incident's
        std::vector<char> open;      // it propagates
    };
    /// The periodic ports' orders at \a omega: their media, kF, β, c.
    bool periodicOrders(double omega, double k0, QString* error);
    /// The equations in the free ones (the periodic conditions' ties, their
    /// phases at kF): T, a_neq by its columns; \a tied false where none is.
    bool periodicMap(Eigen::SparseMatrix<cd>* T, bool* tied, QString* error);

    Media a_media;
    bool a_hz = false;
    std::vector<int> a_eq;          // by dof: equation, -1 none, -3 fixed (0)
    int a_neq = 0;
    std::vector<int> a_condition;   // by boundary: 1 fixed, 2 natural, 3 scattering, 4 port, 5 periodic port, 6 periodic
    std::vector<Periodic> a_periodic;
    std::vector<PeriodicPort> a_pports;
    int a_pfed = -1;
    double a_kF[2] = {0, 0};        // the Floquet wave vector, 1/m
    // Reflectance, transmittance (the fed port's power that comes back, that
    // goes out by the others): NaN where there are none.
    double a_R = NaN, a_T = NaN;
    cd a_r0 = cd(NaN, NaN), a_t0 = cd(NaN, NaN);
    struct Incident {
        bool plane = false;
        Expression amplitude;
        double angle = 0;
    };
    std::vector<Incident> a_incident;   // by boundary
    std::vector<Port> a_ports;
    // The S-matrix found (by excited port's column).
    std::vector<std::vector<cd>> a_S;
    std::vector<int> a_fed;
    double a_omega = 0;
};

bool WaveProblem::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field& f = field();
    const Topology& topo = a_s.topology();
    const Mesh& mesh = a_s.mesh();
    a_hz = a_node->text(QStringLiteral("polarization")) == QLatin1String("hz");
    QString why;
    const QVector<int> domains = resolveSelection(topo, a_s.model(), Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(topo.domains.size(), 0);
    for (int d : domains) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    a_media.setup(a_s, *a_node, f.domains, materials(), mediaHooks());
    Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    coef.c.assign(topo.domains.size(), Expression());
    std::vector<Expression> mur(topo.domains.size()), epsr(topo.domains.size());
    for (std::size_t d = 0; d < a_media.domains.size(); ++d) {
        const Media::Domain& m = a_media.domains[d];
        if (!m.defined) continue;
        if (m.byIndex) {
            coef.c[d] = m.n[0];
            if (m.n[0].isValid()) epsr[d] = Expression::compile(QStringLiteral("(%1)^2").arg(m.n[0].text()), a_s.pointScope());
        } else {
            epsr[d] = m.eps[0][0];
        }
        mur[d] = m.mur[0];
    }
    coef.named[QStringLiteral("mur")] = mur;
    coef.named[QStringLiteral("epsr")] = epsr;
    // Boundaries: conductors, scattering, ports, periodic (the last feature's).
    a_condition.assign(topo.boundaries.size(), 0);
    a_incident.assign(topo.boundaries.size(), Incident());
    a_ports.clear();
    a_periodic.clear();
    a_pports.clear();
    a_pfed = -1;
    std::vector<int> portOf(topo.boundaries.size(), -1), pportOf(topo.boundaries.size(), -1);
    auto inside = [&](int d) { return d >= 0 && f.domains[std::size_t(d)]; };
    // A boundary a feature takes: no longer an earlier one's port.
    auto take = [&](int b, int condition) {
        a_condition[std::size_t(b)] = condition;
        portOf[std::size_t(b)] = -1;
        pportOf[std::size_t(b)] = -1;
    };
    for (const Node& feature : a_node->children) {
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k || k->selection != Level::Boundary) continue;
        const QVector<int> picked = resolveSelection(topo, a_s.model(), Level::Boundary, feature.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(feature.name(), why));
        const QString t = feature.type;
        if (t.endsWith(QLatin1String("/pec")) || t.endsWith(QLatin1String("/pmc"))) {
            // Ez: a conductor fixes it, Hz: a magnetic conductor does.
            const bool pec = t.endsWith(QLatin1String("/pec"));
            for (int b : picked) {
                const Topology::Boundary& bd = topo.boundaries[std::size_t(b)];
                if (feature.isDefault && inside(bd.up) == inside(bd.down)) continue;
                take(b, pec != a_hz ? 1 : 2);
            }
        } else if (t.endsWith(QLatin1String("/scattering"))) {
            Incident in;
            if (feature.text(QStringLiteral("incident")) == QLatin1String("plane")) {
                in.plane = true;
                in.amplitude = compile(feature.text(QStringLiteral("E0")), feature, QStringLiteral("E0"), false);
                const std::optional<double> a = evaluateConstant(feature.text(QStringLiteral("angle")), a_s.parameters().scope, Dim(), &why);
                in.angle = a ? *a * M_PI / 180 : 0;
            }
            for (int b : picked) {
                take(b, 3);
                a_incident[std::size_t(b)] = in;
            }
        } else if (t.endsWith(QLatin1String("/periodicport"))) {
            PeriodicPort port;
            port.name = feature.text(QStringLiteral("name")).trimmed();
            if (port.name.isEmpty()) port.name = QString::number(a_pports.size() + 1);
            for (const PeriodicPort& other : a_pports)
                if (other.name == port.name) problem(tr("%1: another periodic port is named %2").arg(feature.name(), port.name));
            port.excite = feature.flag(QStringLiteral("excite"));
            if (port.excite) {
                if (a_pfed >= 0) problem(tr("%1: a periodic port is fed already (%2): one plane wave comes in").arg(feature.name(), a_pports[std::size_t(a_pfed)].name));
                a_pfed = int(a_pports.size());
                const std::optional<double> a = evaluateConstant(feature.text(QStringLiteral("angle")), a_s.parameters().scope, Dim(), &why);
                if (!a) problem(tr("%1: its angle: %2").arg(feature.name(), why));
                else if (!(std::abs(*a) < 90)) problem(tr("%1: its angle is %2°: less than 90° from the normal").arg(feature.name()).arg(*a));
                else port.angle = *a * M_PI / 180;
                const std::optional<double> e0 = evaluateConstant(feature.text(QStringLiteral("E0")), a_s.parameters().scope, Dim(), &why);
                if (!e0) problem(tr("%1: its amplitude: %2").arg(feature.name(), why));
                else port.E0 = *e0;
            }
            for (int b : picked) {
                take(b, 5);
                pportOf[std::size_t(b)] = int(a_pports.size());
            }
            a_pports.push_back(port);
        } else if (t.endsWith(QLatin1String("/periodic"))) {
            Periodic pc;
            pc.name = feature.name();
            const QString kind = feature.text(QStringLiteral("floquet"));
            pc.floquet = kind == QLatin1String("continuity") ? 0 : kind == QLatin1String("given") ? 2 : 1;
            if (pc.floquet == 2) {
                const std::optional<double> kx = evaluateConstant(feature.text(QStringLiteral("kx")), a_s.parameters().scope, Dim::of(-1), &why);
                if (!kx) problem(tr("%1: kF's x: %2").arg(feature.name(), why));
                const std::optional<double> ky = evaluateConstant(feature.text(QStringLiteral("ky")), a_s.parameters().scope, Dim::of(-1), &why);
                if (!ky) problem(tr("%1: kF's y: %2").arg(feature.name(), why));
                pc.kx = kx.value_or(0);
                pc.ky = ky.value_or(0);
            }
            const std::optional<PeriodicSides> sides = periodicSides(topo, picked, &why);
            if (!sides) {
                problem(tr("%1: %2").arg(feature.name(), why));
                continue;
            }
            pc.shift = sides->shift * topo.unitScale;
            pc.source = sides->source;
            pc.destination = sides->destination;
            for (int b : picked) take(b, 6);
            a_periodic.push_back(pc);
        } else if (t.endsWith(QLatin1String("/port"))) {
            Port port;
            port.name = feature.text(QStringLiteral("name")).trimmed();
            if (port.name.isEmpty()) port.name = QString::number(a_ports.size() + 1);
            for (const Port& other : a_ports)
                if (other.name == port.name) problem(tr("%1: another port is named %2").arg(feature.name(), port.name));
            port.excite = feature.flag(QStringLiteral("excite"));
            for (int b : picked) {
                take(b, 4);
                portOf[std::size_t(b)] = int(a_ports.size());
            }
            a_ports.push_back(port);
        }
    }
    for (int e = 0; e < int(mesh.edges.size()); ++e) {
        const int b = mesh.edges[std::size_t(e)].boundary;
        if (portOf[std::size_t(b)] >= 0) a_ports[std::size_t(portOf[std::size_t(b)])].edges.push_back(e);
        if (pportOf[std::size_t(b)] >= 0) a_pports[std::size_t(pportOf[std::size_t(b)])].edges.push_back(e);
    }
    for (const Port& p : a_ports)
        if (p.edges.empty()) problem(tr("port %1 is on no boundary").arg(p.name));
    for (const PeriodicPort& p : a_pports)
        if (p.edges.empty()) problem(tr("periodic port %1 is on no boundary").arg(p.name));
    if (!a_pports.empty() && !a_ports.empty())
        problem(tr("%1: periodic ports and ports together - a unit cell's periodic ports, or a waveguide's ports").arg(a_node->name()));
    if (!a_pports.empty() && a_pfed < 0) problem(tr("%1: no periodic port is fed: one brings the plane wave in").arg(a_node->name()));
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    // The equations: u fixed at nought where a conductor fixes it.
    const Space& sp = *f.space;
    a_eq.assign(std::size_t(sp.size()), -1);
    for (int t = 0; t < sp.elements(); ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        for (int i = 0; i < sp.perElement(); ++i) a_eq[std::size_t(sp.dofs(t)[i])] = -2;
    }
    for (int e = 0; e < int(mesh.edges.size()); ++e)
        if (a_condition[std::size_t(mesh.edges[std::size_t(e)].boundary)] == 1)
            for (int dof : sp.edgeDofs(e))
                if (dof >= 0 && a_eq[std::size_t(dof)] != -1) a_eq[std::size_t(dof)] = -3;
    a_neq = 0;
    for (int& q : a_eq)
        if (q == -2) q = a_neq++;
    // Periodic conditions: each free node of the destination side tied to
    // the source side at the node moved back by d - the source edge's shape
    // functions there (one node, where the meshes match).
    const double diag = std::hypot(topo.bounds.width(), topo.bounds.height()) * topo.unitScale;
    const double tol = 1e-6 * diag;
    for (Periodic& pc : a_periodic) {
        std::vector<char> isSource(topo.boundaries.size(), 0), isDest(topo.boundaries.size(), 0);
        for (int b : pc.source) isSource[std::size_t(b)] = 1;
        for (int b : pc.destination) isDest[std::size_t(b)] = 1;
        std::vector<int> sourceEdges;
        std::vector<int> dofs;
        for (int e = 0; e < int(mesh.edges.size()); ++e) {
            const int b = mesh.edges[std::size_t(e)].boundary;
            if (isSource[std::size_t(b)]) sourceEdges.push_back(e);
            if (isDest[std::size_t(b)])
                for (int dof : sp.edgeDofs(e))
                    if (dof >= 0 && a_eq[std::size_t(dof)] >= 0 && std::find(dofs.begin(), dofs.end(), dof) == dofs.end()) dofs.push_back(dof);
        }
        int lost = 0;
        for (int dof : dofs) {
            const QPointF at = sp.point(dof) - pc.shift;
            int best = -1;
            double bestDistance = std::numeric_limits<double>::infinity(), bestS = 0;
            for (int e : sourceEdges) {
                const std::array<int, 3> ed = sp.edgeDofs(e);
                const QPointF a = sp.point(ed[0]), d = sp.point(ed[1]) - a;
                const double l2 = d.x() * d.x() + d.y() * d.y();
                if (!(l2 > 0)) continue;
                const double s = std::clamp(((at.x() - a.x()) * d.x() + (at.y() - a.y()) * d.y()) / l2, 0.0, 1.0);
                const double dist = std::hypot(a.x() + s * d.x() - at.x(), a.y() + s * d.y() - at.y());
                if (dist < bestDistance) {
                    best = e;
                    bestDistance = dist;
                    bestS = s;
                }
            }
            if (best < 0 || bestDistance > tol) {
                ++lost;
                continue;
            }
            const std::array<int, 3> ed = sp.edgeDofs(best);
            const double s = bestS;
            double N[3];
            int count = 2;
            if (sp.order() == 1) {
                N[0] = 1 - s;
                N[1] = s;
            } else {
                N[0] = (1 - s) * (1 - 2 * s);
                N[1] = s * (2 * s - 1);
                N[2] = 4 * s * (1 - s);
                count = 3;
            }
            std::vector<std::pair<int, double>> from;
            for (int i = 0; i < count; ++i) {
                if (ed[std::size_t(i)] < 0 || std::abs(N[i]) < 1e-12) continue;
                const int q = a_eq[std::size_t(ed[std::size_t(i)])];
                if (q >= 0) from.push_back({q, N[i]});   // (a fixed node's nought left out)
            }
            pc.ties.push_back({a_eq[std::size_t(dof)], from});
        }
        if (lost > 0) {
            *error = tr("%1: %2 nodes of its destination side have no point of its source side moved onto them").arg(pc.name).arg(lost);
            return false;
        }
        int between = 0;
        for (const auto& tie : pc.ties) between += tie.second.size() > 1 ? 1 : 0;
        if (between > 0)
            a_s.log << tr("Warning: %1: its two sides are meshed apart at %2 of %3 nodes, interpolated there - less exact than a mesh "
                          "the same on both (as made here)")
                           .arg(pc.name)
                           .arg(between)
                           .arg(pc.ties.size());
    }
    // Periodic ports: straight, one period long, along their cell's d.
    for (PeriodicPort& port : a_pports) {
        QPointF lo, hi;
        bool first = true;
        QPointF dir;
        for (int e : port.edges) {
            const std::array<int, 3> ed = sp.edgeDofs(e);
            for (int k = 0; k < 2; ++k) {
                const QPointF x = sp.point(ed[std::size_t(k)]);
                if (first) {
                    lo = hi = x;
                    first = false;
                }
                if (std::hypot(x.x() - lo.x(), x.y() - lo.y()) > std::hypot(hi.x() - lo.x(), hi.y() - lo.y())) hi = x;
            }
        }
        // (the farthest from the first point, then from that: its two ends)
        for (int e : port.edges)
            for (int k = 0; k < 2; ++k) {
                const QPointF x = sp.point(sp.edgeDofs(e)[std::size_t(k)]);
                if (std::hypot(x.x() - hi.x(), x.y() - hi.y()) > std::hypot(lo.x() - hi.x(), lo.y() - hi.y())) lo = x;
            }
        port.length = std::hypot(hi.x() - lo.x(), hi.y() - lo.y());
        if (!(port.length > tol)) {
            *error = tr("periodic port %1 has no length").arg(port.name);
            return false;
        }
        dir = (hi - lo) / port.length;
        if (std::abs(dir.x()) >= std::abs(dir.y()) ? dir.x() < 0 : dir.y() < 0) dir = -dir;
        port.along = dir;
        double crooked = 0;
        for (int e : port.edges)
            for (int k = 0; k < 2; ++k) {
                const QPointF x = sp.point(sp.edgeDofs(e)[std::size_t(k)]) - lo;
                crooked = std::max(crooked, std::abs(x.x() * dir.y() - x.y() * dir.x()));
            }
        if (crooked > tol) {
            *error = tr("periodic port %1 is not straight: a cell's top or bottom, one line").arg(port.name);
            return false;
        }
        const Periodic* cell = nullptr;
        for (const Periodic& pc : a_periodic) {
            const double d = std::hypot(pc.shift.x(), pc.shift.y());
            if (std::abs(pc.shift.x() * dir.y() - pc.shift.y() * dir.x()) <= tol && std::abs(d - port.length) <= 1e-6 * d) cell = &pc;
        }
        if (!cell) {
            *error = tr("periodic port %1 (%2 long) needs a Periodic Condition on its cell's sides, one period along it apart")
                         .arg(port.name, formatQuantity(port.length, Dim::length()));
            return false;
        }
        for (int e : port.edges) {
            int t = -1;
            double xi = 0, eta = 0;
            edgePoint(e, 0.5, &t, &xi, &eta);
            if (t >= 0) port.domain = mesh.domain[std::size_t(t)];
        }
    }
    return true;
}

void WaveProblem::edgePoint(int e, double s, int* t, double* xi, double* eta) const
{
    const Mesh& mesh = a_s.mesh();
    const Field& fld = a_s.fields()[std::size_t(a_fi)];
    const Mesh::Edge& me = mesh.edges[std::size_t(e)];
    *t = -1;
    for (int side : {me.left, me.right})
        if (side >= 0 && fld.domains[std::size_t(mesh.domain[std::size_t(side)])]) {
            *t = side;
            break;
        }
    if (*t < 0) return;
    const std::array<int, 3>& tv = mesh.triangles[std::size_t(*t)];
    const int k = Space::localEdge(tv, me.a, me.b);
    const double r = tv[std::size_t(k)] == me.a ? s : 1 - s;
    *xi = k == 0 ? r : k == 1 ? 1 - r : 0;
    *eta = k == 0 ? 0 : k == 1 ? r : 1 - r;
}

void WaveProblem::planeCoefficients(const Media::At& md, cd P[4], cd* q) const
{
    // Ez: P = diag(1/µyy, 1/µxx), q = εzz; Hz: P from η = ε_t⁻¹ (the
    // in-plane part's inverse) - [ηyy, -ηyx; -ηxy, ηxx] -, q = µzz.
    P[0] = 1.0 / md.mu[1];
    P[1] = P[2] = 0;
    P[3] = 1.0 / md.mu[0];
    if (a_hz) {
        const cd det = md.epsT[0][0] * md.epsT[1][1] - md.epsT[0][1] * md.epsT[1][0];
        const cd eta[4] = {md.epsT[1][1] / det, -md.epsT[0][1] / det, -md.epsT[1][0] / det, md.epsT[0][0] / det};
        P[0] = eta[3];
        P[1] = -eta[2];
        P[2] = -eta[1];
        P[3] = eta[0];
    }
    *q = a_hz ? md.mu[2] : md.epsT[2][2];
}

void WaveProblem::scalars(PointEvaluator& ev, int d, double omega, double k0, const double t[2], cd* across, cd* along, cd* mixed, cd* q) const
{
    cd P[4];
    planeCoefficients(a_media.at(ev, d, omega, k0), P, q);
    const double n[2] = {t[1], -t[0]};
    auto form = [&P](const double a[2], const double b[2]) { return a[0] * (P[0] * b[0] + P[1] * b[1]) + a[1] * (P[2] * b[0] + P[3] * b[1]); };
    *across = form(n, n);
    *along = form(t, t);
    *mixed = form(n, t);
}

bool WaveProblem::portMode(Port& port, double omega, double k0, QString* error)
{
    // Its line's own problem: -(pt e')' - k0² q e = -β² pn e, on the 2D
    // degrees of freedom along it (those fixed at its ends left out) - pt
    // the plane's P along the line, pn across it (an anisotropic medium's,
    // its axes along and across the port; one turned between them mixes
    // the two, nᵀ P t, its mode a tilted one: not a port's).
    const Field& fld = a_s.fields()[std::size_t(a_fi)];
    const Space& sp = *fld.space;
    const Mesh& mesh = a_s.mesh();
    std::map<int, int> local;
    for (int e : port.edges)
        for (int dof : sp.edgeDofs(e))
            if (dof >= 0 && a_eq[std::size_t(dof)] >= 0 && !local.count(dof)) local.emplace(dof, int(local.size()));
    const int n = int(local.size());
    if (n < 1) {
        *error = tr("port %1: nothing free on it").arg(port.name);
        return false;
    }
    Eigen::MatrixXcd K = Eigen::MatrixXcd::Zero(n, n), M = Eigen::MatrixXcd::Zero(n, n);
    PointEvaluator ev(a_s);
    const int ne = sp.order() == 1 ? 2 : 3;
    for (int e : port.edges) {
        const std::array<int, 3> dofs = sp.edgeDofs(e);
        const QPointF a = sp.point(dofs[0]), b = sp.point(dofs[1]);
        const double length = std::hypot(b.x() - a.x(), b.y() - a.y());
        const double tangent[2] = {(b.x() - a.x()) / length, (b.y() - a.y()) / length};
        for (const auto& q : lineRule(sp.order() + 2)) {
            double jac = 0;
            sp.edgePoint(e, q[0], &jac);
            int t = -1;
            double xi = 0, eta = 0;
            edgePoint(e, q[0], &t, &xi, &eta);
            if (t < 0) continue;
            ev.moveTo(t, xi, eta);
            cd p, pt, mixed, qq;
            scalars(ev, mesh.domain[std::size_t(t)], omega, k0, tangent, &p, &pt, &mixed, &qq);
            if (std::abs(mixed) > 1e-9 * std::abs(p)) {
                *error = tr("port %1: its medium's axes are turned in the plane, neither along it nor across it (%2's εxy, with its "
                            "line): its mode would be a tilted one - a port in a medium whose axes lie along and across it")
                             .arg(port.name, a_hz ? tr("ε") : tr("µ"));
                return false;
            }
            const double s = q[0];
            double N[3], dN[3];
            if (ne == 2) {
                N[0] = 1 - s;
                N[1] = s;
                dN[0] = -1;
                dN[1] = 1;
            } else {
                N[0] = (1 - s) * (1 - 2 * s);
                N[1] = s * (2 * s - 1);
                N[2] = 4 * s * (1 - s);
                dN[0] = 4 * s - 3;
                dN[1] = 4 * s - 1;
                dN[2] = 4 - 8 * s;
            }
            const double w = q[1] * jac;
            for (int i = 0; i < ne; ++i) {
                auto ii = local.find(dofs[std::size_t(i)]);
                if (ii == local.end()) continue;
                for (int j = 0; j < ne; ++j) {
                    auto jj = local.find(dofs[std::size_t(j)]);
                    if (jj == local.end()) continue;
                    // (d/ds: d/dq over the length per unit q)
                    K(ii->second, jj->second) += w * (pt * dN[i] * dN[j] / (jac * jac) - k0 * k0 * qq * N[i] * N[j]);
                    M(ii->second, jj->second) += w * p * N[i] * N[j];
                }
            }
        }
    }
    Eigen::ComplexEigenSolver<Eigen::MatrixXcd> es(M.fullPivLu().solve(K));
    int best = -1;
    for (int i = 0; i < n; ++i)
        if (best < 0 || (-es.eigenvalues()(i)).real() > (-es.eigenvalues()(best)).real()) best = i;
    const cd beta2 = -es.eigenvalues()(best);
    if (!(beta2.real() > 0)) {
        *error = tr("port %1: no mode propagates at %2 (its first is cut off)").arg(port.name, formatQuantity(omega / (2 * M_PI), Dim::of(0, 0, -1)));
        return false;
    }
    port.beta = std::sqrt(beta2);
    if (port.beta.real() < 0) port.beta = -port.beta;
    Eigen::VectorXcd e = es.eigenvectors().col(best);
    // ∫ p e e ds = 1 (no conjugate: the bilinear form of the port's condition).
    const cd norm = std::sqrt((e.transpose() * M * e)(0));
    e /= norm;
    // Its sign: ∫ p e ds more than nought (the first mode does not change
    // sign along the port), so that S21's phase does not turn with the mesh.
    const cd along = (M * e).sum();
    if ((std::abs(along.real()) >= std::abs(along.imag()) ? along.real() : along.imag()) < 0) e = -e;
    // c: ∫ p e N_i ds by the 2D equation.
    port.c = Eigen::VectorXcd::Zero(a_neq);
    const Eigen::VectorXcd Me = M * e;
    for (const auto& [dof, i] : local) port.c(a_eq[std::size_t(dof)]) = Me(i);
    return true;
}

// An 8-point Gauss-Legendre rule on [0, 1] (s, weight): a high diffraction
// order's plane wave turns along an edge.
const std::array<std::array<double, 2>, 8>& gauss8()
{
    static const std::array<std::array<double, 2>, 8> rule = [] {
        const double x[4] = {0.1834346424956498, 0.5255324099163290, 0.7966664774136267, 0.9602898564975363};
        const double w[4] = {0.3626837833783620, 0.3137066458778873, 0.2223810344533745, 0.1012285362903763};
        std::array<std::array<double, 2>, 8> r{};
        for (int i = 0; i < 4; ++i) {
            r[std::size_t(2 * i)] = {0.5 - 0.5 * x[i], 0.5 * w[i]};
            r[std::size_t(2 * i + 1)] = {0.5 + 0.5 * x[i], 0.5 * w[i]};
        }
        return r;
    }();
    return rule;
}

bool WaveProblem::periodicOrders(double omega, double k0, QString* error)
{
    if (a_pports.empty()) return true;
    const Field& fld = a_s.fields()[std::size_t(a_fi)];
    const Space& sp = *fld.space;
    const Mesh& mesh = a_s.mesh();
    PointEvaluator ev(a_s);
    // Each port's medium: the same all along it (a plane wave's), its axes
    // along it and across.
    for (PeriodicPort& port : a_pports) {
        bool first = true;
        const double tangent[2] = {port.along.x(), port.along.y()};
        for (int e : port.edges)
            for (const auto& g : lineRule(3)) {
                int t = -1;
                double xi = 0, eta = 0;
                edgePoint(e, g[0], &t, &xi, &eta);
                if (t < 0) continue;
                ev.moveTo(t, xi, eta);
                cd p, pt, mixed, q;
                scalars(ev, mesh.domain[std::size_t(t)], omega, k0, tangent, &p, &pt, &mixed, &q);
                if (std::abs(mixed) > 1e-9 * std::abs(p)) {
                    *error = tr("periodic port %1: its medium's axes are turned in the plane, neither along it nor across it").arg(port.name);
                    return false;
                }
                auto differs = [](cd a, cd b) { return std::abs(a - b) > 1e-9 * std::max(std::abs(a), std::abs(b)); };
                if (first) {
                    port.p = p;
                    port.pt = pt;
                    port.q = q;
                    first = false;
                } else if (differs(p, port.p) || differs(pt, port.pt) || differs(q, port.q)) {
                    *error = tr("periodic port %1: its medium is not the same all along it - the plane waves' half-space: the cell's "
                                "top or bottom in one material")
                                 .arg(port.name);
                    return false;
                }
            }
        if (first) {
            *error = tr("periodic port %1 is on no domain of %2").arg(port.name, a_node->name());
            return false;
        }
    }
    // kF: the fed port's plane wave's along the cell (its index across it).
    const PeriodicPort& fed = a_pports[std::size_t(a_pfed)];
    const cd kIn = k0 * std::sqrt(fed.q / fed.p);
    if (std::abs(kIn.imag()) > 1e-9 * std::abs(kIn.real()))
        a_s.log << tr("Warning: %1: periodic port %2's medium absorbs: the plane wave's angle taken of its index's real part")
                       .arg(a_node->name(), fed.name);
    const double kt0 = kIn.real() * std::sin(fed.angle);
    a_kF[0] = kt0 * fed.along.x();
    a_kF[1] = kt0 * fed.along.y();
    // Each port's orders: those that propagate in any port's medium and ten
    // more each way (the near field's), as many as its edges resolve.
    QStringList said;
    for (PeriodicPort& port : a_pports) {
        const double ktF = a_kF[0] * port.along.x() + a_kF[1] * port.along.y();
        const double kmax = std::sqrt(std::max(0.0, (k0 * k0 * port.q / port.pt).real()));
        const int propagating = int(std::ceil((kmax + std::abs(ktF)) * port.length / (2 * M_PI))) + 1;
        const int M = std::min(propagating + 10, std::max(propagating, int(port.edges.size()) / 2));
        port.m.clear();
        port.kt.clear();
        port.beta.clear();
        QStringList open;
        for (int m = -M; m <= M; ++m) {
            const double kt = ktF + 2 * M_PI * m / port.length;
            const cd beta2 = (k0 * k0 * port.q - port.pt * kt * kt) / port.p;
            cd beta = std::sqrt(beta2);
            if (beta.imag() > 0) beta = -beta;   // e^{-jβξ} out of the cell: going, or dying away
            port.m.push_back(m);
            port.kt.push_back(kt);
            port.beta.push_back(beta);
            if (beta2.real() > 0) open << QStringLiteral("%1 (%2°)").arg(m).arg(std::atan2(kt, beta.real()) * 180 / M_PI, 0, 'f', 2);
        }
        said << tr("%1: %2").arg(port.name, open.isEmpty() ? tr("none") : open.join(QLatin1String(", ")));
        const std::size_t n = port.m.size();
        port.c.assign(n, Eigen::VectorXcd::Zero(a_neq));
        port.dual.assign(n, Eigen::VectorXcd::Zero(a_neq));
        const double root = std::sqrt(port.length);
        for (int e : port.edges) {
            const std::array<int, 3> dofs = sp.edgeDofs(e);
            const int ne = sp.order() == 1 ? 2 : 3;
            for (const auto& g : gauss8()) {
                double jac = 0;
                const QPointF x = sp.edgePoint(e, g[0], &jac);
                const double s = g[0];
                double N[3];
                if (ne == 2) {
                    N[0] = 1 - s;
                    N[1] = s;
                } else {
                    N[0] = (1 - s) * (1 - 2 * s);
                    N[1] = s * (2 * s - 1);
                    N[2] = 4 * s * (1 - s);
                }
                const double w = g[1] * jac;
                const double along = x.x() * port.along.x() + x.y() * port.along.y();
                for (std::size_t k = 0; k < n; ++k) {
                    const cd em = std::exp(-J * port.kt[k] * along) / root;
                    for (int i = 0; i < ne; ++i) {
                        const int q = a_eq[std::size_t(dofs[std::size_t(i)])];
                        if (q < 0) continue;
                        port.c[k](q) += w * N[i] * em;
                        port.dual[k](q) += w * N[i] * std::conj(em);
                    }
                }
            }
        }
    }
    a_s.log << tr("%1: a periodic cell %2 wide, kF %3 (%4° in periodic port %5's medium); the orders that propagate - %6")
                   .arg(a_node->name(), formatQuantity(fed.length, Dim::length()), formatQuantity(kt0, Dim::of(-1)))
                   .arg(fed.angle * 180 / M_PI, 0, 'g', 6)
                   .arg(fed.name, said.join(QLatin1String("; ")));
    return true;
}

bool WaveProblem::periodicMap(Eigen::SparseMatrix<cd>* T, bool* tied, QString* error)
{
    *tied = false;
    if (a_periodic.empty()) return true;
    // Each tied equation: its condition's phase, e^{-j kF·d}, times its
    // source's weights.
    struct Tie {
        cd phase;
        const std::vector<std::pair<int, double>>* from;
    };
    std::vector<int> tieOf(std::size_t(a_neq), -1);
    std::vector<Tie> ties;
    for (const Periodic& pc : a_periodic) {
        double kx = 0, ky = 0;
        if (pc.floquet == 2) {
            kx = pc.kx;
            ky = pc.ky;
        } else if (pc.floquet == 1) {
            kx = a_kF[0];
            ky = a_kF[1];
        }
        const cd phase = std::exp(-J * (kx * pc.shift.x() + ky * pc.shift.y()));
        for (const auto& [q, from] : pc.ties) {
            if (tieOf[std::size_t(q)] >= 0) continue;   // (a corner two conditions tie: the first's)
            tieOf[std::size_t(q)] = int(ties.size());
            ties.push_back({phase, &from});
        }
    }
    if (ties.empty()) return true;
    std::vector<int> freeOf(std::size_t(a_neq), -1);
    int nfree = 0;
    for (int q = 0; q < a_neq; ++q)
        if (tieOf[std::size_t(q)] < 0) freeOf[std::size_t(q)] = nfree++;
    // In the free ones: a source tied in its turn (a corner) followed.
    std::vector<std::vector<std::pair<int, cd>>> sums(static_cast<std::size_t>(a_neq));
    std::vector<char> state(std::size_t(a_neq), 0);   // 1 being summed, 2 done
    const std::function<bool(int)> sum = [&](int q) -> bool {
        if (state[std::size_t(q)] == 2) return true;
        if (state[std::size_t(q)] == 1) return false;
        state[std::size_t(q)] = 1;
        std::map<int, cd> acc;
        if (tieOf[std::size_t(q)] < 0) {
            acc[freeOf[std::size_t(q)]] = 1;
        } else {
            const Tie& t = ties[std::size_t(tieOf[std::size_t(q)])];
            for (const auto& [src, w] : *t.from) {
                if (!sum(src)) return false;
                for (const auto& [col, v] : sums[std::size_t(src)]) acc[col] += t.phase * w * v;
            }
        }
        sums[std::size_t(q)].assign(acc.begin(), acc.end());
        state[std::size_t(q)] = 2;
        return true;
    };
    std::vector<Eigen::Triplet<cd>> trip;
    for (int q = 0; q < a_neq; ++q) {
        if (!sum(q)) {
            *error = tr("%1: its periodic conditions tie a node to itself: one condition for each pair of sides").arg(a_node->name());
            return false;
        }
        for (const auto& [col, v] : sums[std::size_t(q)])
            if (std::abs(v) > 1e-14) trip.emplace_back(q, col, v);
    }
    T->resize(a_neq, nfree);
    T->setFromTriplets(trip.begin(), trip.end());
    T->makeCompressed();
    *tied = true;
    return true;
}

bool WaveProblem::solve(const SolveOptions& o, QString* error)
{
    Field& fld = field();
    if (!(o.harmonic && o.frequency > 0)) {
        *error = tr("%1: waves are solved by a Frequency Domain step, at a frequency more than 0").arg(a_node->name());
        return false;
    }
    const Space& sp = *fld.space;
    const Mesh& mesh = a_s.mesh();
    const double omega = 2 * M_PI * o.frequency;
    a_omega = omega;
    const double k0 = omega / C0;
    for (const QString& w : a_media.outOfRange(omega)) a_s.log << w;
    const int per = sp.perElement();
    std::vector<Eigen::Triplet<cd>> trip;
    Eigen::VectorXcd F = Eigen::VectorXcd::Zero(a_neq);
    PointEvaluator ev(a_s);
    const TriangleRule& rule = triangleRule(sp.order() == 1 ? 2 : 4);
    Shape sh;
    double gx[6], gy[6];
    std::vector<cd> Ke(std::size_t(per * per));
    bool coupled = false;
    for (int t = 0; t < sp.elements(); ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !fld.domains[std::size_t(d)]) continue;
        std::fill(Ke.begin(), Ke.end(), 0);
        const int* dofs = sp.dofs(t);
        for (const auto& q : rule.points) {
            shapeAt(sp.order(), q[0], q[1], sh);
            const Space::Map m = sp.map(t, q[0], q[1]);
            ev.moveTo(t, q[0], q[1]);
            const Media::At md = a_media.at(ev, d, omega, k0);
            if (md.coupled) coupled = true;
            cd P[4], qq;
            planeCoefficients(md, P, &qq);
            const double w = q[2] * std::abs(m.det);
            for (int i = 0; i < per; ++i) Space::gradient(m, sh.dxi[i], sh.deta[i], gx[i], gy[i]);
            for (int i = 0; i < per; ++i)
                for (int j = 0; j < per; ++j)
                    Ke[std::size_t(i * per + j)] +=
                        w * (gx[i] * (P[0] * gx[j] + P[1] * gy[j]) + gy[i] * (P[2] * gx[j] + P[3] * gy[j]) - k0 * k0 * qq * sh.n[i] * sh.n[j]);
        }
        for (int i = 0; i < per; ++i) {
            const int qi = a_eq[std::size_t(dofs[i])];
            if (qi < 0) continue;
            for (int j = 0; j < per; ++j) {
                const int qj = a_eq[std::size_t(dofs[j])];
                if (qj >= 0) trip.emplace_back(qi, qj, Ke[std::size_t(i * per + j)]);
            }
        }
    }
    if (coupled)
        a_s.log << tr("Warning: %1: a medium's ε has xz or yz parts, which would mix Ez and Hz - left out here, waves in the plane take ε's "
                      "in-plane part and εzz (a mode analysis takes them all)")
                       .arg(a_node->name());
    // Scattering boundaries: jk p u v, and what a plane wave brings in.
    const int ne = sp.order() == 1 ? 2 : 3;
    for (int e = 0; e < int(mesh.edges.size()); ++e) {
        const Mesh::Edge& me = mesh.edges[std::size_t(e)];
        if (a_condition[std::size_t(me.boundary)] != 3) continue;
        const Incident& in = a_incident[std::size_t(me.boundary)];
        const std::array<int, 3> dofs = sp.edgeDofs(e);
        int tri = -1;
        double xi0 = 0, eta0 = 0;
        edgePoint(e, 0.5, &tri, &xi0, &eta0);
        if (tri < 0) continue;
        // Its outward normal: away from the triangle it is of.
        const QPointF a = sp.point(dofs[0]), b = sp.point(dofs[1]);
        QPointF nrm(b.y() - a.y(), -(b.x() - a.x()));
        const double len = std::hypot(nrm.x(), nrm.y());
        nrm /= len;
        const auto& tv = mesh.triangles[std::size_t(tri)];
        QPointF centre;
        for (int k = 0; k < 3; ++k) centre += mesh.nodes[std::size_t(tv[std::size_t(k)])] * mesh.unitScale / 3.0;
        if ((centre.x() - a.x()) * nrm.x() + (centre.y() - a.y()) * nrm.y() > 0) nrm = -nrm;
        const double tangent[2] = {-nrm.y(), nrm.x()};
        for (const auto& q : lineRule(sp.order() + 2)) {
            double jac = 0;
            const QPointF x = sp.edgePoint(e, q[0], &jac);
            int t = -1;
            double xi = 0, eta = 0;
            edgePoint(e, q[0], &t, &xi, &eta);
            ev.moveTo(t, xi, eta);
            // (Across it: a wave going straight out, k from nᵀ P n.)
            cd p, pt, mixed, qq;
            scalars(ev, mesh.domain[std::size_t(t)], omega, k0, tangent, &p, &pt, &mixed, &qq);
            const cd k = k0 * std::sqrt(qq / p);
            const double s = q[0];
            double N[3];
            if (ne == 2) {
                N[0] = 1 - s;
                N[1] = s;
            } else {
                N[0] = (1 - s) * (1 - 2 * s);
                N[1] = s * (2 * s - 1);
                N[2] = 4 * s * (1 - s);
            }
            const double w = q[1] * jac;
            cd g = 0;
            if (in.plane) {
                const double amp = in.amplitude.isConstant() ? in.amplitude.constant() : ev.eval(in.amplitude);
                const double dx = std::cos(in.angle), dy = std::sin(in.angle);
                const cd uinc = amp * std::exp(-J * k * (x.x() * dx + x.y() * dy));
                const cd dn = -J * k * (nrm.x() * dx + nrm.y() * dy) * uinc;
                g = p * (dn + J * k * uinc);
            }
            for (int i = 0; i < ne; ++i) {
                const int qi = a_eq[std::size_t(dofs[std::size_t(i)])];
                if (qi < 0) continue;
                F(qi) += w * g * N[i];
                for (int j = 0; j < ne; ++j) {
                    const int qj = a_eq[std::size_t(dofs[std::size_t(j)])];
                    if (qj >= 0) trip.emplace_back(qi, qj, w * J * k * p * N[i] * N[j]);
                }
            }
        }
    }
    // Ports: their modes, jβ c cᵀ each.
    for (Port& port : a_ports) {
        if (!portMode(port, omega, k0, error)) return false;
        std::vector<int> nz;
        for (int i = 0; i < a_neq; ++i)
            if (port.c(i) != cd(0)) nz.push_back(i);
        for (int i : nz)
            for (int j : nz) trip.emplace_back(i, j, J * port.beta * port.c(i) * port.c(j));
    }
    // Periodic ports: each order's condition, jβ_m p c_m ĉ_mᵀ (the field's
    // part in it, ∫ u ê_m, going out as e_m), and the plane wave fed,
    // 2jβ0 p a c_0 (a = E0 √L: e_0's amplitude E0).
    if (!periodicOrders(omega, k0, error)) return false;
    // A condition along the ports whose kF is not their plane wave's: the
    // orders would not be the field's.
    if (a_pfed >= 0)
        for (const Periodic& pc : a_periodic) {
            if (pc.floquet == 1) continue;
            const QPointF along = a_pports[std::size_t(a_pfed)].along;
            if (std::abs(pc.shift.x() * along.y() - pc.shift.y() * along.x()) > 1e-9 * std::hypot(pc.shift.x(), pc.shift.y())) continue;
            const double own = pc.floquet == 2 ? pc.kx * pc.shift.x() + pc.ky * pc.shift.y() : 0.0;
            const double wave = a_kF[0] * pc.shift.x() + a_kF[1] * pc.shift.y();
            if (std::abs(std::remainder(own - wave, 2 * M_PI)) > 1e-6) {
                *error = tr("%1: its kF·d is %2 rad, periodic port %3's plane wave's %4: Floquet, the fed periodic port's")
                             .arg(pc.name)
                             .arg(own, 0, 'g', 6)
                             .arg(a_pports[std::size_t(a_pfed)].name)
                             .arg(wave, 0, 'g', 6);
                return false;
            }
        }
    for (const PeriodicPort& port : a_pports) {
        std::vector<int> nz;
        if (!port.c.empty())
            for (int i = 0; i < a_neq; ++i)
                if (port.c[0](i) != cd(0)) nz.push_back(i);
        for (std::size_t k = 0; k < port.m.size(); ++k) {
            const cd f = J * port.beta[k] * port.p;
            for (int i : nz)
                for (int j : nz) trip.emplace_back(i, j, f * port.c[k](i) * port.dual[k](j));
        }
    }
    cd incident = 0;
    std::size_t specular = 0;
    if (a_pfed >= 0) {
        const PeriodicPort& fp = a_pports[std::size_t(a_pfed)];
        specular = std::size_t(std::find(fp.m.begin(), fp.m.end(), 0) - fp.m.begin());
        incident = fp.E0 * std::sqrt(fp.length);
        F += 2.0 * J * fp.beta[specular] * fp.p * incident * fp.c[specular];
    }
    Eigen::SparseMatrix<cd> A(a_neq, a_neq);
    A.setFromTriplets(trip.begin(), trip.end());
    A.makeCompressed();
    // Periodic conditions: u = T v, v the free nodes' - tested by T's
    // conjugate (the other side's phase), Tᴴ A T v = Tᴴ F.
    Eigen::SparseMatrix<cd> T;
    bool tied = false;
    if (!periodicMap(&T, &tied, error)) return false;
    if (tied) {
        const Eigen::SparseMatrix<cd> Th = T.adjoint();
        A = Eigen::SparseMatrix<cd>(Th * A * T);
        A.makeCompressed();
    }
    Eigen::SparseLU<Eigen::SparseMatrix<cd>> lu;
    lu.analyzePattern(A);
    lu.factorize(A);
    if (lu.info() != Eigen::Success) {
        *error = tr("%1: the matrix is singular - %2").arg(a_node->name(), QString::fromStdString(lu.lastErrorMessage()));
        return false;
    }
    auto solved = [&](const Eigen::VectorXcd& rhs) -> Eigen::VectorXcd {
        if (!tied) return lu.solve(rhs);
        const Eigen::VectorXcd reduced = T.adjoint() * rhs;
        return T * Eigen::VectorXcd(lu.solve(reduced));
    };
    // Fed: each port that is, in turn (the field kept: the first's, the plane
    // wave's when none is).
    a_fed.clear();
    for (int i = 0; i < int(a_ports.size()); ++i)
        if (a_ports[std::size_t(i)].excite) a_fed.push_back(i);
    a_S.assign(a_ports.size(), std::vector<cd>(a_ports.size(), cd(NaN, NaN)));
    Eigen::VectorXcd kept;
    for (std::size_t j = 0; j < a_fed.size() || (j == 0 && a_fed.empty()); ++j) {
        Eigen::VectorXcd rhs = F;
        const int fed = a_fed.empty() ? -1 : a_fed[j];
        if (fed >= 0) {
            const Port& pj = a_ports[std::size_t(fed)];
            rhs += 2.0 * J * pj.beta * pj.c;
            if (j > 0) rhs -= F;   // (a plane wave with the first only)
        }
        const Eigen::VectorXcd x = solved(rhs);
        if (!x.allFinite()) {
            *error = tr("%1: the solution is not finite").arg(a_node->name());
            return false;
        }
        if (j == 0) kept = x;
        if (fed < 0) break;
        for (int i = 0; i < int(a_ports.size()); ++i) {
            const Port& pi = a_ports[std::size_t(i)];
            const cd bi = (pi.c.transpose() * x)(0) - (i == fed ? cd(1) : cd(0));
            a_S[std::size_t(i)][std::size_t(fed)] = bi * std::sqrt(pi.beta / a_ports[std::size_t(fed)].beta);
        }
    }
    // Reflectance and transmittance: of the first port fed, |S_ii|² and the
    // others' |S_ji|² (power waves); of a periodic cell, each order's power
    // over the incident's, Re(p β_m) |b_m|² / Re(p β_0) |a|².
    a_R = a_T = NaN;
    a_r0 = a_t0 = cd(NaN, NaN);
    if (!a_fed.empty()) {
        const int f0 = a_fed.front();
        a_R = std::norm(a_S[std::size_t(f0)][std::size_t(f0)]);
        a_r0 = a_S[std::size_t(f0)][std::size_t(f0)];
        a_T = 0;
        for (int i = 0; i < int(a_ports.size()); ++i)
            if (i != f0) a_T += std::norm(a_S[std::size_t(i)][std::size_t(f0)]);
        if (a_ports.size() == 2) a_t0 = a_S[std::size_t(1 - f0)][std::size_t(f0)];
    }
    if (a_pfed >= 0) {
        const PeriodicPort& fp = a_pports[std::size_t(a_pfed)];
        const double in = (fp.p * fp.beta[specular]).real() * std::norm(incident);
        a_R = a_T = 0;
        for (int i = 0; i < int(a_pports.size()); ++i) {
            PeriodicPort& port = a_pports[std::size_t(i)];
            port.b.assign(port.m.size(), 0);
            port.power.assign(port.m.size(), 0);
            port.open.assign(port.m.size(), 0);
            for (std::size_t k = 0; k < port.m.size(); ++k) {
                cd b = (port.dual[k].transpose() * kept)(0);
                if (i == a_pfed && port.m[k] == 0) b -= incident;
                port.b[k] = b;
                // (an order that does not propagate carries nothing out)
                const bool open = ((k0 * k0 * port.q - port.pt * port.kt[k] * port.kt[k]) / port.p).real() > 0;
                port.open[k] = open;
                port.power[k] = open && in > 0 ? (port.p * port.beta[k]).real() * std::norm(b) / in : 0;
                (i == a_pfed ? a_R : a_T) += port.power[k];
                if (port.m[k] == 0) {
                    if (i == a_pfed) a_r0 = b / incident;
                    else if (a_pports.size() == 2) a_t0 = b / incident;
                }
            }
        }
    }
    // Hz: each triangle's η = ε_t⁻¹ at its middle, for E (re, im of xx, xy,
    // yx, yy).
    fld.extra.clear();
    if (a_hz) {
        fld.extra.assign(std::size_t(8 * sp.elements()), NaN);
        for (int t = 0; t < sp.elements(); ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (d < 0 || !fld.domains[std::size_t(d)]) continue;
            ev.moveTo(t, 1.0 / 3, 1.0 / 3);
            const Media::At md = a_media.at(ev, d, omega, k0);
            const cd det = md.epsT[0][0] * md.epsT[1][1] - md.epsT[0][1] * md.epsT[1][0];
            const cd eta[4] = {md.epsT[1][1] / det, -md.epsT[0][1] / det, -md.epsT[1][0] / det, md.epsT[0][0] / det};
            for (int k = 0; k < 4; ++k) {
                fld.extra[std::size_t(8 * t + 2 * k)] = eta[k].real();
                fld.extra[std::size_t(8 * t + 2 * k + 1)] = eta[k].imag();
            }
        }
    }
    const int nd = sp.size();
    fld.values.assign(std::size_t(2 * nd), 0.0);
    for (int dof = 0; dof < nd; ++dof) {
        const int q = a_eq[std::size_t(dof)];
        if (q < 0) continue;
        fld.values[std::size_t(dof)] = kept(q).real();
        fld.values[std::size_t(nd + dof)] = kept(q).imag();
    }
    fld.terminals.clear();
    for (const Port& p : a_ports) {
        Field::Terminal t;
        t.name = p.name;
        t.potential = p.beta.real();
        t.potentialIm = p.beta.imag();
        fld.terminals.push_back(t);
    }
    fld.solved = true;
    return true;
}

void WaveProblem::setGlobals()
{
    const QString p = field().tag + QLatin1Char('.');
    for (int i = 0; i < int(a_ports.size()); ++i) {
        const QString a = a_ports[std::size_t(i)].name;
        a_s.setGlobal(p + QStringLiteral("beta_") + a, a_ports[std::size_t(i)].beta.real(), Dim::of(-1), tr("port %1's mode, its propagation constant").arg(a));
        for (int j : a_fed) {
            const QString b = a_ports[std::size_t(j)].name;
            const cd v = a_S[std::size_t(i)][std::size_t(j)];
            const bool shortNames = a.size() == 1 && b.size() == 1;
            const QString base = shortNames ? p + QStringLiteral("S") + a + b : p + QStringLiteral("S_") + a + QLatin1Char('_') + b;
            const QString what = tr("S-parameter %1 %2").arg(a, b);
            a_s.setGlobal(base, v.real(), Dim(), what + tr(", its real part"));
            a_s.setGlobal(base + QStringLiteral("_im"), v.imag(), Dim(), what + tr(", its imaginary part"));
            a_s.setGlobal(base + QStringLiteral("dB"), 20 * std::log10(std::abs(v)), Dim(), what + tr(" in dB"));
            a_s.setGlobal(base + QStringLiteral("_abs"), std::abs(v), Dim(), what + tr(", its magnitude"));
            a_s.setGlobal(base + QStringLiteral("_arg"), std::arg(v) * 180 / M_PI, Dim(), what + tr(", its phase in degrees"));
        }
    }
    // Reflectance, transmittance, absorptance: the first port fed's, or the
    // periodic cell's.
    if (!std::isnan(a_R)) {
        const QString of = a_pfed >= 0 ? tr("the plane wave's power") : tr("the power fed");
        a_s.setGlobal(p + QStringLiteral("R"), a_R, Dim(), tr("reflectance: %1 that comes back").arg(of));
        a_s.setGlobal(p + QStringLiteral("T"), a_T, Dim(), tr("transmittance: %1 that goes out by the other ports").arg(of));
        a_s.setGlobal(p + QStringLiteral("A"), 1 - a_R - a_T, Dim(), tr("absorptance: %1 neither reflected nor transmitted, 1 - R - T").arg(of));
    }
    if (a_pfed < 0) return;
    // Each order that propagates: its share of the incident power, R_m at
    // the fed port, T_m at the other (T_<port>_m where there are more).
    const auto order = [](int m) { return m < 0 ? QStringLiteral("n%1").arg(-m) : QString::number(m); };
    for (int i = 0; i < int(a_pports.size()); ++i) {
        const PeriodicPort& port = a_pports[std::size_t(i)];
        const bool fed = i == a_pfed;
        double total = 0;
        for (std::size_t k = 0; k < port.m.size() && k < port.open.size(); ++k) {
            if (!port.open[k]) continue;
            total += port.power[k];
            const QString name = fed ? QStringLiteral("R_") + order(port.m[k])
                                 : a_pports.size() == 2 ? QStringLiteral("T_") + order(port.m[k])
                                                        : QStringLiteral("T_%1_%2").arg(port.name, order(port.m[k]));
            const QString what = fed ? tr("reflectance into order %1").arg(port.m[k])
                                     : tr("transmittance into order %1 at periodic port %2").arg(port.m[k]).arg(port.name);
            a_s.setGlobal(p + name, port.power[k], Dim(), what);
        }
        if (!fed && a_pports.size() > 2) a_s.setGlobal(p + QStringLiteral("T_") + port.name, total, Dim(), tr("transmittance at periodic port %1").arg(port.name));
    }
    // The zeroth orders' fields over the incident's, at the ports' lines
    // (their phase along them taken from the same origin).
    a_s.setGlobal(p + QStringLiteral("r_0"), a_r0.real(), Dim(), tr("reflection coefficient, order 0, its real part"));
    a_s.setGlobal(p + QStringLiteral("r_0_im"), a_r0.imag(), Dim(), tr("reflection coefficient, order 0, its imaginary part"));
    if (!std::isnan(a_t0.real())) {
        a_s.setGlobal(p + QStringLiteral("t_0"), a_t0.real(), Dim(), tr("transmission coefficient, order 0, its real part"));
        a_s.setGlobal(p + QStringLiteral("t_0_im"), a_t0.imag(), Dim(), tr("transmission coefficient, order 0, its imaginary part"));
    }
    a_s.setGlobal(p + QStringLiteral("kF"), std::hypot(a_kF[0], a_kF[1]), Dim::of(-1), tr("the Floquet wave number along the cell"));
}

} // namespace

WaveFields waveFieldsAt(PointEvaluator& e, int f)
{
    WaveFields out;
    const Solution& s = e.solution();
    const Field& fld = s.fields()[std::size_t(f)];
    double u = 0, ux = 0, uy = 0, v = 0, vx = 0, vy = 0;
    e.component(f, 0, u, ux, uy);
    e.component(f, 1, v, vx, vy);
    if (!std::isfinite(u)) return out;
    out.u = cd(u, v);
    out.ux = cd(ux, vx);
    out.uy = cd(uy, vy);
    const double omega = 2 * M_PI * s.frequency();
    if (!(omega > 0)) return out;
    double mur = e.coefficient(f, QStringLiteral("mur"));
    if (!std::isfinite(mur) || mur <= 0) mur = 1;
    double epsr = e.coefficient(f, QStringLiteral("epsr"));
    if (!std::isfinite(epsr) || epsr == 0) epsr = 1;
    const bool hz = fld.variable == QLatin1String("Hz");
    if (!hz) {
        // E = ẑ Ez; H = j/(ωµ) ∇×E = j/(ωµ) (∂y Ez, -∂x Ez, 0).
        const cd f0 = J / (omega * Mu0 * mur);
        out.E[2] = out.u;
        out.H[0] = f0 * out.uy;
        out.H[1] = -f0 * out.ux;
    } else {
        // H = ẑ Hz; E = (1/jωε0) η ∇×H, ∇×H = (∂y Hz, -∂x Hz, 0), η = ε⁻¹ in the
        // plane (the triangle's, as solved; else 1/εr).
        const cd f0 = -J / (omega * 8.8541878188e-12);
        const int t = e.triangle();
        cd eta[4] = {1.0 / epsr, 0, 0, 1.0 / epsr};
        if (t >= 0 && fld.extra.size() >= std::size_t(8 * (t + 1)) && std::isfinite(fld.extra[std::size_t(8 * t)]))
            for (int k = 0; k < 4; ++k) eta[k] = cd(fld.extra[std::size_t(8 * t + 2 * k)], fld.extra[std::size_t(8 * t + 2 * k + 1)]);
        out.H[2] = out.u;
        out.E[0] = f0 * (eta[0] * out.uy - eta[1] * out.ux);
        out.E[1] = f0 * (eta[2] * out.uy - eta[3] * out.ux);
    }
    out.defined = true;
    return out;
}

std::unique_ptr<Problem> makeWaveProblem(Solution& solution, int field)
{
    return std::make_unique<WaveProblem>(solution, field);
}

MediumAt modeMediumAt(PointEvaluator& e, int f)
{
    MediumAt m;
    const Field& field = e.solution().fields()[std::size_t(f)];
    const int t = e.triangle();
    if (t < 0 || field.extra.size() < std::size_t(3 + MediumAt::PerTriangle * (t + 1))) return m;
    const double* x = &field.extra[std::size_t(3 + MediumAt::PerTriangle * t)];
    if (!std::isfinite(x[0])) return m;
    m.defined = true;
    m.omega = field.extra[2] * C0;
    m.n = x[0];
    m.kappaGap = x[1];
    m.kappaEA = x[2];
    m.kappaEA0 = x[3];
    m.dnCarriers = x[13];
    m.alphaCarriers = x[14];
    for (int pol = 0; pol < 2; ++pol) {
        m.dnWells[pol] = x[15 + pol];
        m.alphaWells[pol] = x[17 + pol];
    }
    for (int c = 0; c < 3; ++c) m.lossEps[c][c] = x[4 + c];
    const int pairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};
    for (int k = 0; k < 3; ++k) {
        const cd v(x[7 + 2 * k], x[8 + 2 * k]);
        m.lossEps[pairs[k][0]][pairs[k][1]] = v;
        m.lossEps[pairs[k][1]][pairs[k][0]] = std::conj(v);
    }
    return m;
}

ModeFields modeFieldsAt(PointEvaluator& e, int f)
{
    ModeFields out;
    const Solution& s = e.solution();
    const Field& fld = s.fields()[std::size_t(f)];
    const int t = e.triangle();
    if (t < 0 || !fld.edges || fld.extra.size() < 3) return out;
    const int d = e.domain();
    if (d < 0 || !fld.domains[std::size_t(d)]) return out;
    const EdgeSpace& es = *fld.edges;
    const Space& ns = *fld.space;
    const int ne = es.size(), nn = ns.size();
    if (int(fld.values.size()) < 2 * ne + 2 * nn) return out;
    const AffineMap am = affineMap(ns, t);
    EdgeSpace::Basis b;
    es.basis(t, am, e.xi(), e.eta(), b);
    cd ex = 0, ey = 0, curl = 0;
    for (int i = 0; i < b.count; ++i) {
        const int dof = es.dofs(t)[i];
        const cd v(fld.values[std::size_t(dof)], fld.values[std::size_t(ne + dof)]);
        ex += b.vx[i] * v;
        ey += b.vy[i] * v;
        curl += b.curl[i] * v;
    }
    Shape sh;
    shapeAt(ns.order(), e.xi(), e.eta(), sh);
    cd ez = 0, ezx = 0, ezy = 0;
    for (int i = 0; i < sh.count; ++i) {
        const int dof = ns.dofs(t)[i];
        const cd v(fld.values[std::size_t(2 * ne + dof)], fld.values[std::size_t(2 * ne + nn + dof)]);
        ez += sh.n[i] * v;
        ezx += (sh.dxi[i] * am.grad[1][0] + sh.deta[i] * am.grad[2][0]) * v;
        ezy += (sh.dxi[i] * am.grad[1][1] + sh.deta[i] * am.grad[2][1]) * v;
    }
    const cd beta(fld.extra[0], fld.extra[1]);
    const double k0 = fld.extra[2];
    const double omega = k0 * C0;
    double mur = e.coefficient(f, QStringLiteral("mur"));
    if (!std::isfinite(mur) || mur <= 0) mur = 1;
    // ∇×E with e^{-jβz}: (∂y Ez + jβ Ey, -jβ Ex - ∂x Ez, ∂x Ey - ∂y Ex);
    // H = j/(ωµ) ∇×E.
    const cd f0 = J / (omega * Mu0 * mur);
    out.E[0] = ex;
    out.E[1] = ey;
    out.E[2] = ez;
    out.H[0] = f0 * (ezy + J * beta * ey);
    out.H[1] = f0 * (-J * beta * ex - ezx);
    out.H[2] = f0 * curl;
    out.defined = true;
    return out;
}

std::unique_ptr<Problem> makeModeProblem(Solution& solution, int field)
{
    return std::make_unique<ModeProblem>(solution, field);
}

ResonatorFields resonatorFieldsAt(PointEvaluator& e, int f)
{
    ResonatorFields out;
    const Solution& s = e.solution();
    const Field& fld = s.fields()[std::size_t(f)];
    const int t = e.triangle();
    if (t < 0 || !fld.edges || fld.extra.size() < 3) return out;
    const int d = e.domain();
    if (d < 0 || !fld.domains[std::size_t(d)]) return out;
    const EdgeSpace& es = *fld.edges;
    const Space& ns = *fld.space;
    const int ne = es.size(), nn = ns.size();
    if (int(fld.values.size()) < 2 * ne + 2 * nn) return out;
    const Mesh& mesh = s.mesh();
    const AffineMap am = affineMap(ns, t);
    // r there; on the axis (or nearly), the fields a little inside its
    // triangle - its curl's 1/r parts their limits.
    double xi = e.xi(), eta = e.eta();
    auto rAt = [&](double a, double b) {
        const std::array<int, 3>& v = mesh.triangles[std::size_t(t)];
        return ((1 - a - b) * mesh.nodes[std::size_t(v[0])].x() + a * mesh.nodes[std::size_t(v[1])].x() + b * mesh.nodes[std::size_t(v[2])].x())
               * mesh.unitScale;
    };
    double r = rAt(xi, eta);
    const double size = std::sqrt(std::abs(am.det));
    if (r < 1e-6 * size) {
        xi += 1e-3 * (1.0 / 3 - xi);
        eta += 1e-3 * (1.0 / 3 - eta);
        r = rAt(xi, eta);
    }
    if (!(r > 0)) return out;
    EdgeSpace::Basis b;
    es.basis(t, am, xi, eta, b);
    cd er = 0, ez = 0, curl = 0;
    for (int i = 0; i < b.count; ++i) {
        const int dof = es.dofs(t)[i];
        const cd v(fld.values[std::size_t(dof)], fld.values[std::size_t(ne + dof)]);
        er += b.vx[i] * v;
        ez += b.vy[i] * v;
        curl += b.curl[i] * v;
    }
    Shape sh;
    shapeAt(ns.order(), xi, eta, sh);
    cd chi = 0, chir = 0, chiz = 0;
    for (int i = 0; i < sh.count; ++i) {
        const int dof = ns.dofs(t)[i];
        const cd v(fld.values[std::size_t(2 * ne + dof)], fld.values[std::size_t(2 * ne + nn + dof)]);
        chi += sh.n[i] * v;
        chir += (sh.dxi[i] * am.grad[1][0] + sh.deta[i] * am.grad[2][0]) * v;
        chiz += (sh.dxi[i] * am.grad[1][1] + sh.deta[i] * am.grad[2][1]) * v;
    }
    const cd k(fld.extra[0], fld.extra[1]);
    const double m = fld.extra[2];
    const cd omega = k * C0;
    double mur = e.coefficient(f, QStringLiteral("mur"));
    if (!std::isfinite(mur) || mur <= 0) mur = 1;
    // E_φ = j χ/r; ∇×E = (-j(m E_z + ∂z χ)/r, j(m E_r + ∂r χ)/r, ∂z E_r - ∂r E_z)
    // along r, z, φ; H = j/(ωµ) ∇×E.
    const cd f0 = J / (omega * Mu0 * mur);
    out.E[0] = er;
    out.E[1] = ez;
    out.E[2] = J * chi / r;
    out.H[0] = f0 * (-J * (m * ez + chiz) / r);
    out.H[1] = f0 * (J * (m * er + chir) / r);
    out.H[2] = f0 * (-curl);
    if (fld.extra.size() > std::size_t(4 + t)) out.eps = fld.extra[std::size_t(4 + t)];
    out.defined = true;
    return out;
}

std::unique_ptr<Problem> makeResonatorProblem(Solution& solution, int field)
{
    return std::make_unique<ResonatorProblem>(solution, field);
}

} // namespace qucs_s::fem::detail
