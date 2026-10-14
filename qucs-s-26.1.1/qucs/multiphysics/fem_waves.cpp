/*
 * fem_waves.cpp - electromagnetic waves: a cross-section's modes (the
 *                 fields e^{-jβz} along it, full-vector: Et on Nédélec's
 *                 edge elements, Ez on Lagrange's - Lee, Sun and Cendes'
 *                 formulation, A x = -β² B x, solved by a shift and invert
 *                 about the effective index guessed) and waves in the plane
 *                 (Ez or Hz: -∇·(P ∇u) - k0² q u = 0) with ports and their
 *                 S-parameters, scattering boundaries; in media of a complex
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
#include "fem_optics.h"
#include "fem_problem.h"

#include <Eigen/Dense>

#include <QJsonArray>

#include <algorithm>
#include <cmath>
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
        Expression carriersN, carriersP;   // the plasma dispersion's (1/m³)
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
        // Free carriers (silicon): Soref and Bennett's at 1550 nm, as λ² (Drude's).
        double plasma = 0, plasmaKappa = 0;
        if (m.carriersN.isValid() || m.carriersP.isValid()) {
            const double lambda = omega > 0 ? 2 * M_PI * 299792458.0 / omega : 1.55e-6;
            const double scale = (lambda / 1.55e-6) * (lambda / 1.55e-6);
            const double N = std::max(0.0, value(m.carriersN, 0)) * 1e-6, P = std::max(0.0, value(m.carriersP, 0)) * 1e-6;   // (cm⁻³)
            plasma = -(8.8e-22 * N + 8.5e-18 * std::pow(P, 0.8)) * scale;
            const double alpha = (8.5e-18 * N + 6.0e-18 * P) * scale * 100;   // (1/m)
            plasmaKappa = alpha * lambda / (4 * M_PI);
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
        // ε in the crystal's axes.
        cd ec[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        if (m.byIndex) {
            out.n = value(m.n[0], 1.0) + warm[0] + plasma;
            out.kappaGap = value(m.kappa[0], 0.0) + out.kappaEA;
            for (int k = 0; k < 3; ++k) {
                const cd n(value(m.n[k].isValid() ? m.n[k] : m.n[0], 1.0) + warm[k] + plasma,
                           -value(m.kappa[k].isValid() ? m.kappa[k] : m.kappa[0], 0.0) - plasmaKappa - out.kappaEA);
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
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) out.epsT[i][j] *= s[0] * s[1] / (s[i] * s[j]);
            const cd f[3] = {s[1] / s[0], s[0] / s[1], s[0] * s[1]};
            for (int k = 0; k < 3; ++k) {
                out.mu[k] *= f[k];
                out.eps[k] = out.epsT[k][k];
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
            if (feat->flag(QStringLiteral("plasma"))) {
                if (!m.byIndex) c.problem(tr("%1: the plasma dispersion adds to a refractive index (its constitutive relation n)").arg(feat->name()));
                m.carriersN = c.compile(feat->text(QStringLiteral("Ncarriers")), *feat, QStringLiteral("Ncarriers"));
                m.carriersP = c.compile(feat->text(QStringLiteral("Pcarriers")), *feat, QStringLiteral("Pcarriers"));
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
    void setGlobals() override;

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
    // - or the other polarization's.)
    const bool polarized = o.polarization == 1 || o.polarization == 2;
    int dropped = 0, crossed = 0, products = 0, restarts = 0;
    for (int asked = a_tracking ? std::max(2 * wanted + 4, wanted + 6) : layers || polarized ? 2 * wanted + 4 : wanted;; asked *= 2) {
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
            if (polarized && (o.polarization == 1 ? !(te >= 0.5) : !(te < 0.5))) {
                ++crossed;
                continue;
            }
            a_modes.push_back({beta, x, a_tracking ? shareIn(x, beta, tracked) : NaN, te});
        }
        if ((!layers && !polarized) || int(a_modes.size()) >= wanted || asked >= 96 || asked >= a_n - 2) break;
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
    if (dropped) a_s.log << tr("%1: %2 modes of the absorbing layers left out").arg(a_node->name()).arg(dropped);
    if (crossed)
        a_s.log << tr("%1: %2 %3 modes left out").arg(a_node->name()).arg(crossed).arg(o.polarization == 1 ? tr("TM") : tr("TE"));
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

    Media a_media;
    bool a_hz = false;
    std::vector<int> a_eq;          // by dof: equation, -1 none, -3 fixed (0)
    int a_neq = 0;
    std::vector<int> a_condition;   // by boundary: 1 fixed, 2 natural, 3 scattering
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
    // Boundaries: conductors, scattering, ports (the last feature's).
    a_condition.assign(topo.boundaries.size(), 0);
    a_incident.assign(topo.boundaries.size(), Incident());
    a_ports.clear();
    std::vector<int> portOf(topo.boundaries.size(), -1);
    auto inside = [&](int d) { return d >= 0 && f.domains[std::size_t(d)]; };
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
                a_condition[std::size_t(b)] = pec != a_hz ? 1 : 2;
                portOf[std::size_t(b)] = -1;
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
                a_condition[std::size_t(b)] = 3;
                a_incident[std::size_t(b)] = in;
                portOf[std::size_t(b)] = -1;
            }
        } else if (t.endsWith(QLatin1String("/port"))) {
            Port port;
            port.name = feature.text(QStringLiteral("name")).trimmed();
            if (port.name.isEmpty()) port.name = QString::number(a_ports.size() + 1);
            for (const Port& other : a_ports)
                if (other.name == port.name) problem(tr("%1: another port is named %2").arg(feature.name(), port.name));
            port.excite = feature.flag(QStringLiteral("excite"));
            for (int b : picked) {
                a_condition[std::size_t(b)] = 4;
                portOf[std::size_t(b)] = int(a_ports.size());
            }
            a_ports.push_back(port);
        }
    }
    for (int e = 0; e < int(mesh.edges.size()); ++e) {
        const int b = mesh.edges[std::size_t(e)].boundary;
        if (portOf[std::size_t(b)] >= 0) a_ports[std::size_t(portOf[std::size_t(b)])].edges.push_back(e);
    }
    for (const Port& p : a_ports)
        if (p.edges.empty()) problem(tr("port %1 is on no boundary").arg(p.name));
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
    Eigen::SparseMatrix<cd> A(a_neq, a_neq);
    A.setFromTriplets(trip.begin(), trip.end());
    A.makeCompressed();
    Eigen::SparseLU<Eigen::SparseMatrix<cd>> lu;
    lu.analyzePattern(A);
    lu.factorize(A);
    if (lu.info() != Eigen::Success) {
        *error = tr("%1: the matrix is singular - %2").arg(a_node->name(), QString::fromStdString(lu.lastErrorMessage()));
        return false;
    }
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
        const Eigen::VectorXcd x = lu.solve(rhs);
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

} // namespace qucs_s::fem::detail
