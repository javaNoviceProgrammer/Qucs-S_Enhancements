/*
 * fem_quantum.cpp - quantum wells: the effective-mass Schrödinger equation
 *                   -∇·(ħ²/2m ∇ψ) + V ψ = E ψ of electrons, heavy holes and
 *                   light holes (BenDaniel and Duke's: 1/m ∂ψ/∂n continuous
 *                   across a heterojunction), in 1D along the growth axis
 *                   through the layers (quadratic elements of their own) or
 *                   in 2D over the cross-section (wires; dots about an
 *                   axis); its bands from the materials' compounds (strained
 *                   on a substrate: Bir and Pikus), a field along the axis
 *                   (the quantum-confined Stark effect) or another physics'
 *                   potential; each carrier's lowest states by a shift and
 *                   invert below them; the transitions' energies and
 *                   overlaps, and an optical spectrum - absorption, or gain
 *                   with carriers in the wells
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_edge.h"
#include "fem_exciton.h"
#include "fem_line.h"
#include "fem_materials.h"
#include "fem_problem.h"
#include "fem_process_p.h"
#include "fem_results.h"

#include <Eigen/Dense>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseLU>

#include <QScopeGuard>

#include <algorithm>
#include <complex>
#include <functional>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

namespace qucs_s::fem::detail {

namespace {

constexpr double Q = 1.602176634e-19;     // C
constexpr double KB = 1.380649e-23;       // J/K
constexpr double Hbar = 1.054571817e-34;  // J s
constexpr double M0 = 9.1093837015e-31;   // kg
constexpr double C0 = 299792458.0;        // m/s
constexpr double HB2M = 0.0380998212;     // ħ²/2m0 in eV nm²
constexpr int Carriers = 3;               // electrons, heavy holes, light holes
/// The band data kept with the field: its header's length.
constexpr int Header = 10;

/// Catmull and Rom's cubic through values on k from 0, dk apart, at k: the
/// first held before it, the last past it.
template <class T>
T cubicAt(const std::vector<T>& v, double dk, double k)
{
    const int m = int(v.size());
    if (m == 1 || !(dk > 0) || !(k > 0)) return v.front();
    const double u = k / dk;
    if (u >= m - 1) return v.back();
    const int i = int(u);
    const double t = u - i;
    const T& p0 = v[std::size_t(std::max(0, i - 1))];
    const T& p1 = v[std::size_t(i)];
    const T& p2 = v[std::size_t(i + 1)];
    const T& p3 = v[std::size_t(std::min(m - 1, i + 2))];
    return T(0.5 * (2.0 * p1 + t * (p2 - p0) + (t * t) * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) + (t * t * t) * (3.0 * (p1 - p2) + p3 - p0)));
}

/// A domain's bands (eV) and masses (m0) as solved.
struct Band {
    bool on = false;
    bool user = false;
    double Ec = 0, Ehh = 0, Elh = 0;   // strained, before the field's potential
    double me = 0, mhhZ = 0, mlhZ = 0;  // along the growth axis
    double mhhP = 0, mlhP = 0;          // across it
    double Ep = 0;                      // Kane's energy (eV)
    double nr = NaN;                    // refractive index
    double epsr = NaN;                  // static permittivity (the excitons')
    // The k·p valence bands': Luttinger's γ1-γ3, the split-off Δ (eV), and
    // what strain puts the Z orbital's edge above the X and Y orbitals'
    // (eV: 3 Qε, Chuang's Qε = -b (εxx - εzz)).
    double g1 = NaN, g2 = NaN, g3 = NaN, delta = NaN, dZ = 0;
    double strain = 0;                  // in the plane
    // Wurtzite (c along the growth axis): Chuang and Chang's 6x6 - A1-A6
    // (ħ²/2m0), Δ1 = Δcr, Δ2 = Δ3 = Δso/3, strain's λε and θε on its
    // bands (eV); its heavy holes' and light holes' edges over the k·p's
    // reference, the conduction band's strain shift (eV: a user's added to
    // its Ec and Ev); the electron's mass across c and Kane's Ep along it
    // (TM); its polarization along c (C/m²: spontaneous and piezoelectric).
    bool wz = false;
    double A[6] = {0, 0, 0, 0, 0, 0};
    double dcr = 0, dso = 0, lambdaE = 0, thetaE = 0;
    double hhOver = 0, lhOver = 0, ecShift = 0, kpRef = 0;
    double meP = NaN, EpZ = NaN, P = 0;
    Expression userEc, userEv;          // (as given: of x and y)
    std::vector<ValleySet> valleys;     // the electrons' valley sets (none: one, me's)
    // As read, for an export (quantumLayers): the edges unstrained (eV; a
    // user's when constant), the lattice constant (m) and what strains it
    // (eV, Pa) - a zinc blende's.
    double Ec0 = NaN, Ev0 = NaN, a0 = NaN, ac = NaN, av = NaN, bDef = NaN, c11 = NaN, c12 = NaN;
};

/// A point of integration: its domain, weight (nm, nm² or nm³ about an
/// axis), coordinate along the growth axis (m), each carrier's potential
/// (eV: Ec, -Ehh, -Elh), its element's equations and their shape
/// functions' values and gradients along and across the axis (1/nm).
struct Point {
    int d = -1;
    int t = -1;              // 1D: its triangle (-1 none found) and place in it
    double rx = 0, ry = 0;
    double w = 0, a = 0;
    double pot[Carriers] = {0, 0, 0};
    int n = 0;
    int eq[6];
    double N[6], ga[6], gc[6];
};

/// A state found: its energy (eV; a hole's below its band), its
/// wavefunction by equation (ψᵀ M ψ = 1), where it is along the axis (m),
/// whether it is bound (below the barriers' band edge), its mass across the
/// axis, Kane's energy and refractive index where it is.
struct State {
    double E = 0;
    Eigen::VectorXd x;
    double pos = 0;
    bool bound = false;
    double mPar = 0, Ep = 0, EpZ = 0, nr = 0;
    int valley = -1;     // an electron's valley set (-1: one valley, me's)
    double g = 1;        // its valleys (each with spin's two)
    bool top = false;    // its ladder's highest found
};

const char* const CarrierName[Carriers] = {"e", "hh", "lh"};

double Inf()
{
    return std::numeric_limits<double>::infinity();
}

double fermi(double x)
{
    if (x > 40) return std::exp(-x);
    if (x < -40) return 1;
    return 1 / (1 + std::exp(x));
}

/// ln(1 + e^x), safe both ways.
double softplus(double x)
{
    return x > 30 ? x + std::log1p(std::exp(-x)) : std::log1p(std::exp(x));
}

/// The Fermi-Dirac integral of order -1/2, normalized - (2/√π) ∫0^∞
/// dt/(1 + e^(t² - η)), e^η where the carriers are few: a wire's carriers
/// along it (Simpson's rule).
double fermiMinusHalf(double eta)
{
    if (eta < -30) return std::exp(eta);
    const double T = std::sqrt(std::max(eta, 0.0) + 40);
    const int m = 400;
    const double h = T / m;
    double sum = 0;
    for (int j = 0; j <= m; ++j) {
        const double t = j * h, w = (j == 0 || j == m) ? 1 : (j % 2 ? 4 : 2);
        sum += w * fermi(t * t - eta);
    }
    return 2 / std::sqrt(M_PI) * sum * h / 3;
}

/// The Coulomb attraction of a 1s exciton's in-plane envelope, √(2/π)/λ
/// e^(-ρ/λ), at a separation s along the axis, times λ (q²/4πε aside):
/// g(u) = 4 ∫0^∞ t e^(-2t)/√(t² + u²) dt at u = s/λ - 2 at u = 0 (the strictly
/// 2D exciton's 4 Ry), 1/u far off. Tabulated once over ln u (t = u sinh θ,
/// Simpson's rule).
double excitonG(double u)
{
    constexpr int N = 4000;
    constexpr double lo = -13.815510557964274, hi = 9.210340371976184;   // ln 1e-6, ln 1e4
    static const std::vector<double> table = [] {
        std::vector<double> g(N + 1);
        for (int k = 0; k <= N; ++k) {
            const double uk = std::exp(lo + (hi - lo) * k / N);
            const double top = std::asinh(40 / uk);
            constexpr int m = 2000;
            const double h = top / m;
            double sum = 0;
            for (int j = 0; j <= m; ++j) {
                const double x = uk * std::sinh(j * h), w = (j == 0 || j == m) ? 1 : (j % 2 ? 4 : 2);
                sum += w * x * std::exp(-2 * x);
            }
            g[std::size_t(k)] = 4 * sum * h / 3;
        }
        return g;
    }();
    if (!(u > 0)) return 2;
    const double x = std::log(u);
    if (x <= lo) return table.front();
    if (x >= hi) return 1 / u - 0.75 / (u * u * u);
    const double f = (x - lo) / (hi - lo) * N;
    const int k = std::min(N - 1, int(f));
    return table[std::size_t(k)] + (f - k) * (table[std::size_t(k) + 1] - table[std::size_t(k)]);
}

/// An energy (J) as the log says it: in eV, meV or µeV - not volts.
QString electronVolts(double joules)
{
    const double v = joules / 1.602176634e-19, a = std::abs(v);
    if (a >= 1 || a == 0) return formatNumber(v, 5) + QStringLiteral(" eV");
    if (a >= 1e-3) return formatNumber(v * 1e3, 4) + QStringLiteral(" meV");
    return formatNumber(v * 1e6, 4) + QStringLiteral(" µeV");
}

/// E1(x) = ∫ e^(-t)/t dt from x to ∞ (x > 0): its series below 1, Lentz's
/// continued fraction above.
double expIntegralE1(double x)
{
    if (!(x > 0)) return Inf();
    if (x <= 1) {
        double sum = 0, term = 1;
        for (int k = 1; k < 80; ++k) {
            term *= -x / k;
            sum -= term / k;
            if (std::abs(term / k) < 1e-17 * std::max(1.0, std::abs(sum))) break;
        }
        return -0.57721566490153286 - std::log(x) + sum;
    }
    double b = x + 1, c = 1e300, d = 1 / b, h = d;
    for (int i = 1; i < 400; ++i) {
        const double a = -double(i) * i;
        b += 2;
        d = 1 / (a * d + b);
        c = b + a / c;
        const double del = c * d;
        h *= del;
        if (std::abs(del - 1) < 1e-16) break;
    }
    return h * std::exp(-x);
}

/// The two-parameter exciton's Coulomb term, v(x, γ) = 4 ∫ t e^(-2√(t² +
/// γ²x²))/√(t² + x²) dt (t = ρ/λ, x = s/λ): excitonG at γ = 0. A table in
/// ln x (1e-6 to 1e4) and γ (0 to 1.5), Simpson's in t = x sinh u.
double excitonV(double x, double g)
{
    constexpr int NX = 1000, NG = 60;
    constexpr double lo = -13.815510557964274, hi = 9.210340371976184, gmax = 1.5;
    static const std::vector<double> table = [] {
        std::vector<double> v(std::size_t((NX + 1) * (NG + 1)));
        for (int i = 0; i <= NX; ++i) {
            const double xi = std::exp(lo + (hi - lo) * i / NX), top = std::asinh(40 / xi);
            constexpr int m = 300;
            const double h = top / m;
            for (int j = 0; j <= NG; ++j) {
                const double gj = gmax * j / NG;
                double sum = 0;
                for (int k = 0; k <= m; ++k) {
                    const double sh = std::sinh(k * h), w = (k == 0 || k == m) ? 1 : (k % 2 ? 4 : 2);
                    sum += w * xi * sh * std::exp(-2 * xi * std::sqrt(sh * sh + gj * gj));
                }
                v[std::size_t(i * (NG + 1) + j)] = 4 * sum * h / 3;
            }
        }
        return v;
    }();
    if (!(g > 0)) return excitonG(x);
    if (!(x > 0)) return excitonG(0);
    const double lx = std::log(x);
    if (lx >= hi) return g * x > 20 ? 0.0 : excitonG(x) * std::exp(-2 * g * x);
    // (Catmull and Rom's cubic both ways: smooth, so that no node of the
    // table is a minimum of its own.)
    const double fx = std::clamp((lx - lo) / (hi - lo) * NX, 0.0, double(NX)), fg = std::clamp(g / gmax * NG, 0.0, double(NG));
    const int i = std::min(NX - 1, int(fx)), j = std::min(NG - 1, int(fg));
    auto weights = [](double t, double w[4]) {
        const double t2 = t * t, t3 = t2 * t;
        w[0] = (-t3 + 2 * t2 - t) / 2;
        w[1] = (3 * t3 - 5 * t2 + 2) / 2;
        w[2] = (-3 * t3 + 4 * t2 + t) / 2;
        w[3] = (t3 - t2) / 2;
    };
    double wx[4], wg[4];
    weights(fx - i, wx);
    weights(fg - j, wg);
    double v = 0;
    for (int a = 0; a < 4; ++a) {
        const int ii = std::clamp(i - 1 + a, 0, NX);
        for (int b = 0; b < 4; ++b) v += wx[a] * wg[b] * table[std::size_t(ii * (NG + 1) + std::clamp(j - 1 + b, 0, NG))];
    }
    return v;
}

class QuantumProblem : public Problem
{
public:
    QuantumProblem(Solution& solution, int field) : Problem(solution, field) {}
    friend QuantumLayers detail::quantumLayers(Solution& solution, int field);

    bool setup(QString* error) override;
    void initialize() override;
    bool solve(const SolveOptions& options, QString* error) override;
    void setGlobals() override;
    int solveModes(const SolveOptions& options, int count, double guess, QString* error) override;
    void selectMode(int k) override;
    int solveSpectrum(const SpectrumOptions& o, QString* error) override;
    void selectSpectrum(int k) override;
    double wellField() const override { return a_F; }
    bool setWellField(double F) override
    {
        a_F = F;
        return true;
    }
    bool spectrumAt(double E, double out[4]) const override
    {
        // (Its energy nearest E: one of them, asked for.)
        std::size_t best = a_spec.E.size();
        for (std::size_t k = 0; k < a_spec.E.size(); ++k)
            if (best == a_spec.E.size() || std::abs(a_spec.E[k] - E) < std::abs(a_spec.E[best] - E)) best = k;
        if (best == a_spec.E.size() || std::abs(a_spec.E[best] - E) > 1e-9 * E || best >= a_spec.dnTE.size()) return false;
        out[0] = a_spec.te[best];
        out[1] = a_spec.tm[best];
        out[2] = a_spec.dnTE[best];
        out[3] = a_spec.dnTM[best];
        return true;
    }
    int solveBands(const SpectrumOptions& o, QString* error) override;
    void selectBands(int k) override;

private:
    /// 1D: the layers the line crosses, its elements and nodes.
    bool buildLine(QString* error);
    /// The points of integration with their potentials; the band data kept
    /// with the field.
    bool prepare(QString* error);
    /// The bands at the evaluator's point of domain \a d, \a a along the
    /// axis (m): Ec, Ehh, Elh (eV), the field's and the potential's in.
    void bandsAt(PointEvaluator& ev, int d, double a, double out[Carriers]) const;
    /// Carrier \a c's \a count lowest states.
    bool states(int c, int count, QString* error);
    /// ∫ ψa ψb f(domain, along the axis) over the points.
    double integral(const Eigen::VectorXd& a, const Eigen::VectorXd& b, const std::function<double(int, double)>& f) const;
    /// Carrier \a c's mass in domain \a d along the axis or across it (an
    /// electron's of the valley set being solved, a_set), and its band's
    /// offset there (an electron's valley set's edge above the band's, eV).
    double mass(int c, int d, bool along) const;
    double offset(int c, int d) const;
    /// One ladder of carrier \a c's states (the valley set a_set's).
    bool ladder(int c, int count, QString* error);
    int a_sets = 0;    // the electrons' valley sets (0: one valley, me's)
    int a_set = -1;    // the one solved
    /// |ψ|² in SI (1/m, 1/m², 1/m³) from the integration's units.
    double densityScale() const;
    /// The carriers in the states found, by degree of freedom (1/m³): each
    /// state's |ψ|² times its free directions' density of states, filled to
    /// the Fermi level a_etaN above the conduction band (electrons) or
    /// a_etaP below the valence band (holes) there.
    void densities();
    Expression a_etaN, a_etaP;
    std::vector<double> a_density[2];   // electrons, holes
    // A well's carriers per area (1/m²; along its line): each state's, by
    // carrier, and the electrons' in each valley set.
    std::vector<double> a_sheet[Carriers];
    std::vector<double> a_sheetValley;

    bool a_1d = true;
    int a_axis = 1;   // 0 x, 1 y
    bool a_on[Carriers] = {true, true, true};
    int a_order = 2;
    double a_T = 300, a_F = 0, a_s0 = 0, a_drawn = 0.05, a_h1d = 1e-10;
    // The line's two ends one point (1D): a superlattice's period, its
    // states those at its mini-zone's centre (kz 0) - M periods drawn, M
    // points of it (kz 2πj/ML). Else hard walls there.
    bool a_periodic = false;
    // Wurtzite's polarization (1D): 0 none, 1 periodic (no drop across the
    // physics), 2 no field at its ends; its sign along the axis (1 the metal
    // face's, -1 the nitrogen face's); each point's element, and the energy
    // (eV) it adds there.
    int a_polarization = 0;
    double a_polarity = 1;
    std::vector<int> a_pointElement;
    std::vector<double> a_Upol;
    // The carriers' charge screening it (C/m³ at the points: a spectrum's),
    // and whether a solve is in its loop (its field said once, after).
    std::vector<double> a_rho;
    bool a_screening = false;
    /// A spectrum's carriers screen the polarization: the states and their
    /// charge q(p - n) in turn with its potential, the charge mixed, till
    /// the potential moves less than 0.1 meV (60 turns at most).
    bool screen(const SpectrumOptions& o, QString* error);
    /// The polarization's: each element's field F = (D - P)/ε0εr along the
    /// axis - D the displacement, by the interfaces' bound charges -ΔP and
    /// the free charges \a rho (C/m³ at the points; none: none) -, D such
    /// that the potential drops nothing across the physics (periodic) or that
    /// its first layer has none; the bands moved by ∫F from the physics'
    /// middle (as the field's). The energy (eV) at each point; its field in
    /// each domain (V/m, their mean) into \a fields when asked.
    std::vector<double> polarizationEnergy(const std::vector<double>* rho, std::vector<double>* fields = nullptr) const;
    Expression a_V;
    std::vector<Band> a_bands;   // by domain
    // An intermixed well's (a process flow's Intermix): the flow's
    // composition, each domain's material on its grid and its bands'
    // edges as made (Ec, Ev, eV), its band set.
    std::shared_ptr<const ProcessDopants> a_mix;
    std::vector<int> a_mixMaterial;
    std::vector<std::array<double, 2>> a_mixNominal;
    std::vector<BandSet> a_mixSet;
    double a_mixScale = 1;
    /// The edges' move at \a p (the geometry's unit) of domain \a d: the
    /// alloy's there less the layer's.
    bool mixShift(QPointF p, int d, double* dEc, double* dEv) const;
    std::vector<int> a_flux;     // boundaries whose wavefunction is free
    // 1D: the line (m) and its nodes (m from its start), its elements' domains (-1 a gap).
    QPointF a_S, a_E;
    double a_L = 0;
    std::vector<double> a_z;
    std::vector<int> a_elemDomain;
    // The equations by degree of freedom (2D) or node (1D): -1 none.
    std::vector<int> a_eq;
    int a_neq = 0;
    std::vector<Point> a_points;
    std::vector<State> a_states[Carriers];
    double a_barrier[Carriers] = {NaN, NaN, NaN};   // Ec's least, Ehh's and Elh's greatest where the physics ends
    std::vector<char> a_active;                     // by domain: of the smallest gap (the wells)
    double a_activeSize = 0;                        // their length (m) or area (m²)
    int a_selected = -1;
    /// The first states' overlaps |<e_i|h_j>|² (by hole carrier), and the
    /// direct transition: the lowest of the electron and heavy hole states
    /// whose overlap is at least half the largest - a well's own e1-hh1, where
    /// a field puts the stack's lowest and highest states in different wells.
    std::vector<std::vector<double>> a_overlap[Carriers];
    double a_directE = NaN, a_directO = NaN;
    struct Spectrum {
        std::vector<double> E, te, tm;   // J, 1/m
        double Fc = NaN, Fv = NaN, size = 0, nr = 0;
        double per = 0;   // what the absorption is per (m): the wells', the stack's, a length
        bool carriers = false;
        // The e1-hh1 exciton's binding energy and peak (J), its radius (m).
        bool excitons = false;
        double Eb = NaN, Ex = NaN, radius = NaN, gammaX = NaN;
        // The field across the wells (V/m), the broadening (J); the
        // spontaneous emission (1/(m³ s J)) and its total over the window
        // (1/(m³ s)); the Kramers-Kronig index of the window's absorption;
        // the absorption of the transitions well above each energy (1/m).
        double field = 0, gamma = 0, Rsp = NaN;
        std::vector<double> rsp, gspTE, gspTM, dnTE, dnTM, dnTEtail, dnTMtail, farTE, farTM;
    };
    /// A 1s exciton (1D wells): bound by Eb, λ across (its decay in the plane), γ
    /// drawing the electron and hole together along the axis (0: Bastard's),
    /// |F(0)|² its in-plane function's at the origin (1/m²; 2/(πλ²) at γ 0).
    struct Exciton {
        double Eb = 0, radius = NaN, gamma = 0, F0 = NaN;
    };
    /// Of states e and h (carrier \a c), with \a two parameters or Bastard's one.
    Exciton exciton(const State& e, const State& h, int c, double mr, double epsr, bool two) const;
    /// The same of two densities |ψ|² (1/nm) at the points, their masses
    /// along the axis' inverses (1/m0) there.
    Exciton exciton(const std::function<double(const Point&)>& e, const std::function<double(const Point&)>& h,
                    const std::function<double(const Point&)>& ie, const std::function<double(const Point&)>& ih, double mr, double epsr,
                    bool two) const;
    /// ψ at a point, of a real (complex) vector by equation (from \a offset).
    double at(const Point& pt, const Eigen::VectorXd& x) const;
    std::complex<double> at(const Point& pt, const Eigen::VectorXcd& x, int offset) const;
    int a_valence = 0;   // 0: parabolic holes; 4, 6 or 8: the k·p bands' size (8: Kane's, the conduction band's S too)
    int a_inplane = 0;   // the in-plane k: 0 axial, 1 along [100], 2 along [110], 3 averaged over directions
    /// The k·p valence bands (1D wells) along an in-plane direction (θ from
    /// [100]; its share of the average): at each in-plane k (1/m) the
    /// highest states' energies (eV, an electron's) and envelopes in X↑ Y↑
    /// Z↑ X↓ Y↓ Z↓ (by component, then equation; x† M x = 1).
    struct KpBands {
        double theta = 0, weight = 1;
        std::vector<double> k;
        std::vector<std::vector<double>> E;
        std::vector<std::vector<Eigen::VectorXcd>> x;
        // The 8x8's: the conduction states' energies (eV, from the lowest)
        // and vectors (X↑ Y↑ Z↑ X↓ Y↓ Z↓ S↑ S↓, 8n), the valence states'
        // S parts (S↑ S↓, 2n).
        std::vector<std::vector<double>> C;
        std::vector<std::vector<Eigen::VectorXcd>> xc, xs;
        // Each state along k (a spectrum's subband): order[m][j] the state at
        // k m - its index in E[m], x[m] - continuing k = 0's state j, by their
        // overlaps through crossings (a high field's states in a barrier's
        // notch cross the wells' subbands); orderC the 8x8's conduction
        // states'. E, x and C stay by energy.
        std::vector<std::vector<int>> order, orderC;
        std::size_t at(std::size_t m, std::size_t j) const { return m < order.size() && j < order[m].size() ? std::size_t(order[m][j]) : j; }
        std::size_t atC(std::size_t m, std::size_t i) const { return m < orderC.size() && i < orderC[m].size() ? std::size_t(orderC[m][i]) : i; }
    };
    std::vector<KpBands> a_kps;   // each direction's: one, or the average's
    QString kpDirection() const;   // the in-plane k's, said
    QString kpSpuriousSaid() const;   // the 8x8's spurious states left out, said ("; ..." or none)
    /// Solves them at \a nk values of k from 0 to \a kmax (1/m): the
    /// highest \a count states at each (Kramers' pairs both); the 8x8's
    /// lowest \a conduction conduction states too (Kramers' pairs both).
    bool kpBands(int count, double kmax, int nk, QString* error, int conduction = 0);
    QStringList a_kpRescaled;   // the 8x8's Ep rescaled, said once a solve
    // A spectrum's or Valence Bands' solveModes (their levels their own),
    // not a States step's; a States step's k·p levels at k = 0 (6x6, 8x8).
    int a_inner = 0;
    bool kpLevels(QString* said, QString* error);
    // The 8x8's spurious states left out at k = 0 (of each kind: conduction,
    // valence), and their bands' shares, said.
    int a_kpSpurious[2] = {0, 0};
    int a_kpSpuriousKept[2] = {0, 0}, a_kpKeptKs[2] = {0, 0};   // (taken all the same, too few else, and at how many values of k: electrons, holes)
    int a_kpSpuriousKs = 0, a_kpKs = 0;    // (the values of k with any left out, of all)
    QStringList a_kpSpuriousShares;
    Spectrum a_spec;
    int a_specSelected = -1;
    int a_bandsSelected = -1;   // a Valence Bands step's point
};

double QuantumProblem::mass(int c, int d, bool along) const
{
    const Band& b = a_bands[std::size_t(d)];
    if (c == 0) {
        if (a_set >= 0 && std::size_t(a_set) < b.valleys.size()) {
            const ValleySet& v = b.valleys[std::size_t(a_set)];
            return along ? v.mz : v.md;
        }
        return along || !std::isfinite(b.meP) ? b.me : b.meP;
    }
    if (c == 1) return along ? b.mhhZ : b.mhhP;
    return along ? b.mlhZ : b.mlhP;
}

double QuantumProblem::offset(int c, int d) const
{
    const Band& b = a_bands[std::size_t(d)];
    return c == 0 && a_set >= 0 && std::size_t(a_set) < b.valleys.size() ? b.valleys[std::size_t(a_set)].dE : 0.0;
}

double QuantumProblem::densityScale() const
{
    if (a_1d) return 1e9;
    if (a_s.axisymmetric()) return 1e27 / (2 * M_PI);
    return 1e18;
}

bool QuantumProblem::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field& f = field();
    const Model& model = a_s.model();
    const Topology& topo = a_s.topology();
    const Scope& constants = a_s.parameters().scope;
    QString why;
    const QVector<int> domains = resolveSelection(topo, model, Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(topo.domains.size(), 0);
    for (int d : domains) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    a_1d = a_node->text(QStringLiteral("space")) != QLatin1String("2d");
    a_axis = a_node->text(QStringLiteral("axis")) == QLatin1String("x") ? 0 : 1;
    a_order = f.space->order();
    a_on[0] = a_node->flag(QStringLiteral("electrons"));
    a_on[1] = a_node->flag(QStringLiteral("heavy"));
    a_on[2] = a_node->flag(QStringLiteral("light"));
    if (!a_on[0] && !a_on[1] && !a_on[2]) problem(tr("%1 solves no carrier").arg(a_node->name()));
    {
        const QString v = a_node->text(QStringLiteral("valence"));
        a_valence = v == QLatin1String("kp8") ? 8 : v == QLatin1String("kp6") ? 6 : v == QLatin1String("kp4") ? 4 : 0;
        const QString in = a_node->text(QStringLiteral("inplane"));
        a_inplane = in == QLatin1String("100") ? 1 : in == QLatin1String("110") ? 2 : in == QLatin1String("average") ? 3 : 0;
        if (a_valence && !a_1d) problem(tr("%1: the k·p valence bands are a well's (1D): a wire's holes stay parabolic").arg(a_node->name()));
    }
    // (About an axis, a line along it - y, z - is planar wells across it, a
    // VCSEL's: their plane the r-φ plane; along r there are none.)
    if (a_1d && a_s.axisymmetric() && a_axis == 0)
        problem(tr("%1: about an axis its wells lie across it - their line along the axis (y), or 2D for dots round it").arg(a_node->name()));
    // (What is read in: the parameters, then - its temperature known - its
    // materials' T too.)
    const Scope* reading = &constants;
    auto number = [&](const QString& text, const Dim& dim, const QString& what) {
        const std::optional<double> v = evaluateConstant(text, *reading, dim, &why);
        if (!v) {
            problem(tr("%1: %2: %3").arg(a_node->name(), what, why));
            return NaN;
        }
        return *v;
    };
    // An energy in eV: a plain number or a voltage as eV, joules as such.
    auto energy = [&](const QString& text, const QString& what) {
        const Expression e = Expression::compile(text, *reading);
        if (!e.isValid() || !e.isConstant() || !std::isfinite(e.constant())) {
            problem(tr("%1: %2: %3").arg(a_node->name(), what, e.isValid() ? tr("it must be a finite constant") : e.error()));
            return NaN;
        }
        return e.dim() == Dim::of(2, 1, -2) ? e.constant() / Q : e.constant();
    };
    a_T = number(a_node->text(QStringLiteral("T")), Dim::temperature(), tr("its temperature"));
    // (Not a number: said already.)
    if (!std::isnan(a_T) && !(a_T > 0)) problem(tr("%1: its temperature must be more than 0").arg(a_node->name()));
    // (Solved as given, but said: 1e9 K solved without a word - the hunt of
    // 21 October, N7.)
    else if (a_T > 2000)
        a_s.log << tr("Warning: %1: a temperature of %2 K, past the material library's fits (they hold to about 1000 K)")
                       .arg(a_node->name(), formatNumber(a_T, 4));
    // Its materials' texts read T, the wells' temperature (the library's
    // gaps, densities of states and mobilities are expressions of it).
    const MaterialScope materialsAtT(constants, a_T > 0 ? a_T : 300), materialsAt300(constants, 300);
    reading = &materialsAtT;
    a_F = number(a_node->text(QStringLiteral("F")), Dim::of(1, 1, -3, -1), tr("its field"));
    if (!std::isfinite(a_F)) a_F = 0;
    {
        const QString pol = a_node->text(QStringLiteral("polarization"));
        a_polarization = pol == QLatin1String("periodic") ? 1 : pol == QLatin1String("ends") ? 2 : 0;
        a_polarity = a_node->text(QStringLiteral("polarity")) == QLatin1String("nitrogen") ? -1 : 1;
        if (a_polarization && !a_1d) problem(tr("%1: the polarization's field is a well's (1D)").arg(a_node->name()));
    }
    a_drawn = energy(a_node->text(QStringLiteral("drawn")), tr("the height its waves are drawn"));
    if (!std::isfinite(a_drawn)) a_drawn = 0.05;
    a_periodic = a_1d && a_node->text(QStringLiteral("ends")) == QLatin1String("periodic");
    // (A line's alone: in 2D, walls - said; a file or a tool can set it,
    // the dialog hides it - the hunt of 21 October, N1.)
    if (!a_1d && a_node->text(QStringLiteral("ends")) == QLatin1String("periodic"))
        a_s.log << tr("Warning: %1: periodic ends are a line's (1D): the walls bound this one").arg(a_node->name());
    if (a_1d) {
        a_h1d = number(a_node->text(QStringLiteral("h1d")), Dim::length(), tr("its element size"));
        if (!(a_h1d > 0)) {
            problem(tr("%1: its element size along the line must be more than 0").arg(a_node->name()));
            a_h1d = 1e-10;
        }
    }
    const QString vText = a_node->text(QStringLiteral("V")).trimmed();
    a_V = vText.isEmpty() || vText == QLatin1String("0") ? Expression() : compile(vText, *a_node, QStringLiteral("V"));
    // The carriers' Fermi levels from the band edges (another physics': a
    // semiconductor's semi.Efn - semi.Ec): empty, no carriers counted.
    auto optional = [&](const QString& key) {
        const QString t = a_node->text(key).trimmed();
        return t.isEmpty() ? Expression() : compile(t, *a_node, key);
    };
    a_etaN = optional(QStringLiteral("etan"));
    a_etaP = optional(QStringLiteral("etap"));
    // The substrate's lattice constant: a library material's, or a length.
    double substrate = NaN;
    const QString sub = a_node->text(QStringLiteral("substrate")).trimmed();
    if (!sub.isEmpty()) {
        QString compound;
        double sx = 0, sy = 0;
        for (const Node& m : model.materials().children)
            if (m.name().compare(sub, Qt::CaseInsensitive) == 0 || m.text(QStringLiteral("library")).compare(sub, Qt::CaseInsensitive) == 0) {
                compound = materialProperty(m, QStringLiteral("compound"));
                sx = evaluateConstant(materialProperty(m, QStringLiteral("x")), constants, Dim()).value_or(0);
                sy = evaluateConstant(materialProperty(m, QStringLiteral("y")), constants, Dim()).value_or(0);
                const QString a0 = materialProperty(m, QStringLiteral("a0"), &constants);
                if (!a0.trimmed().isEmpty() && compound.isEmpty()) substrate = number(a0, Dim::length(), tr("its substrate's lattice constant"));
            }
        if (compound.isEmpty() && !std::isfinite(substrate))
            if (const std::optional<MaterialEntry> e = findMaterial(sub)) {
                compound = e->properties.value(QStringLiteral("compound")).toString();
                sx = evaluateConstant(e->properties.value(QStringLiteral("x")).toString(), constants, Dim()).value_or(0);
                sy = evaluateConstant(e->properties.value(QStringLiteral("y")).toString(), constants, Dim()).value_or(0);
            }
        if (compound.isEmpty() && !std::isfinite(substrate)) {
            // A compound's name (GaAs) or a length.
            if (bandParameters(sub, 0, 0, a_T)) compound = sub;
            else substrate = number(sub, Dim::length(), tr("its substrate (a material or a lattice constant)"));
        }
        if (!compound.isEmpty()) {
            const std::optional<BandParameters> p = bandParameters(compound, sx, sy, a_T, &why);
            if (p) substrate = p->a0;
            else problem(tr("%1: its substrate: %2").arg(a_node->name(), why));
        }
        if (!(substrate > 0)) problem(tr("%1: its substrate's lattice constant must be more than 0").arg(a_node->name()));
    }
    // Each domain's bands and masses.
    const std::vector<const Node*> material = materials();
    a_bands.assign(topo.domains.size(), Band());
    std::vector<const Node*> modelOf(topo.domains.size(), nullptr);
    a_flux.clear();
    for (const Node& feature : a_node->children) {
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k) continue;
        const QVector<int> picked = resolveSelection(topo, model, k->selection, feature.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(feature.name(), why));
        if (feature.type == QLatin1String("quantum/material"))
            for (int d : picked) modelOf[std::size_t(d)] = &feature;
        else if (feature.type == QLatin1String("quantum/flux"))
            for (int b : picked) a_flux.push_back(b);
    }
    // A wurtzite layer's bands (Chuang and Chang's): at k = 0 the heavy
    // holes' Δ1 + Δ2 + λε + θε over the k·p's reference, the light and
    // crystal-field holes' the 2x2 of Δ1 - Δ2 + λε + θε and λε coupled by √2
    // Δ3 (λε = D1 εzz + D2 (εxx + εyy), θε = D3 εzz + D4 (εxx + εyy)); the
    // conduction band moved by a1 εzz + a2 (εxx + εyy); the polarization
    // Psp + e31 (εxx + εyy) + e33 εzz. A library's Ev its top unstrained, a
    // user's the reference (its edges' offsets added to its Ec and Ev). Its
    // parabolic holes' masses (heavy and light alike): along c 1/(A1 + A3),
    // across 1/(A2 + A4) - the 6x6 is the band's.
    auto wurtzite = [&](Band& b, const WurtziteParameters& w, double exx, double ezz, bool library) {
        b.wz = true;
        std::copy(w.A, w.A + 6, b.A);
        b.dcr = w.dcr;
        b.dso = w.dso;
        b.delta = w.dso;
        b.lambdaE = w.D[0] * ezz + 2 * w.D[1] * exx;
        b.thetaE = w.D[2] * ezz + 2 * w.D[3] * exx;
        b.strain = exx;
        const double d1 = w.dcr, d2 = w.dso / 3;
        auto edges = [&](double lam, double th, double& hh, double& lh) {
            hh = d1 + d2 + lam + th;
            const double g = d1 - d2 + lam + th, m = 0.5 * (g + lam), h = 0.5 * (g - lam);
            lh = m + std::sqrt(h * h + 2 * d2 * d2);
        };
        double hh0 = 0, lh0 = 0, hh = 0, lh = 0;
        edges(0, 0, hh0, lh0);
        edges(b.lambdaE, b.thetaE, hh, lh);
        b.kpRef = hh;
        b.ecShift = w.a1 * ezz + 2 * w.a2 * exx;
        if (library) {
            const double ref = w.Ev - std::max(hh0, lh0);
            b.Ec = w.Ev + w.Eg + b.ecShift;
            b.Ehh = ref + hh;
            b.Elh = ref + lh;
            b.me = w.mez;
            b.meP = w.mep;
            b.Ep = w.Ep;
            b.EpZ = w.Ep;
            b.epsr = w.epsr;
            b.nr = w.n;
            b.ecShift = 0;
        } else {
            b.hhOver = hh;
            b.lhOver = lh;
        }
        b.mhhZ = b.mlhZ = -1 / (w.A[0] + w.A[2]);
        b.mhhP = b.mlhP = -1 / (w.A[1] + w.A[3]);
        b.P = w.Psp + 2 * w.e31 * exx + w.e33 * ezz;
    };
    for (int d : domains) {
        Band& b = a_bands[std::size_t(d)];
        const Node* mf = modelOf[std::size_t(d)];
        const Node* mat = material[std::size_t(d)];
        const QString where = tr("domain %1 (%2)").arg(d + 1).arg(topo.objectNames.value(topo.domains[std::size_t(d)].owner));
        b.on = true;
        // (Wurtzite's: the record, its strain in the plane and along c, its
        // Ev the top's or the reference's.)
        std::optional<WurtziteParameters> wz;
        double wzExx = 0, wzEzz = 0;
        bool wzLibrary = true;
        if (mf && mf->text(QStringLiteral("source")) == QLatin1String("user")) {
            b.user = true;
            b.userEc = compile(mf->text(QStringLiteral("Ec")), *mf, QStringLiteral("Ec"));
            b.userEv = compile(mf->text(QStringLiteral("Ev")), *mf, QStringLiteral("Ev"));
            for (auto [e, out] : {std::pair{&b.userEc, &b.Ec0}, std::pair{&b.userEv, &b.Ev0}})
                if (e->isConstant()) *out = e->dim() == Dim::of(2, 1, -2) ? e->constant() / Q : e->constant();
            b.me = number(mf->text(QStringLiteral("me")), Dim(), tr("%1's electron mass").arg(where));
            b.mhhZ = number(mf->text(QStringLiteral("mhh")), Dim(), tr("%1's heavy hole mass").arg(where));
            b.mlhZ = number(mf->text(QStringLiteral("mlh")), Dim(), tr("%1's light hole mass").arg(where));
            const QString hp = mf->text(QStringLiteral("mhhp")).trimmed(), lp = mf->text(QStringLiteral("mlhp")).trimmed();
            b.mhhP = hp.isEmpty() ? b.mhhZ : number(hp, Dim(), where);
            b.mlhP = lp.isEmpty() ? b.mlhZ : number(lp, Dim(), where);
            b.Ep = energy(mf->text(QStringLiteral("Ep")), tr("%1's Kane energy").arg(where));
            // (Luttinger's γ1, γ2 of the masses along the axis; γ3 and Δ as
            // given, γ3 else γ2.)
            b.g1 = (1 / b.mhhZ + 1 / b.mlhZ) / 2;
            b.g2 = (1 / b.mlhZ - 1 / b.mhhZ) / 4;
            const QString g3 = mf->text(QStringLiteral("gamma3")).trimmed(), so = mf->text(QStringLiteral("Delta")).trimmed();
            b.g3 = g3.isEmpty() ? b.g2 : number(g3, Dim(), tr("%1's γ3").arg(where));
            b.delta = so.isEmpty() ? NaN : energy(so, tr("%1's split-off energy").arg(where));
            b.nr = number(mf->text(QStringLiteral("nr")), Dim(), tr("%1's refractive index").arg(where));
            const QString eps = mf->text(QStringLiteral("epsilonr")).trimmed();
            b.epsr = eps.isEmpty() ? b.nr * b.nr : number(eps, Dim(), tr("%1's permittivity").arg(where));
            if (mf->text(QStringLiteral("crystal")) != QLatin1String("wurtzite") && !mf->text(QStringLiteral("exx")).trimmed().isEmpty()) {
                // Zinc blende strained as given: Bir and Pikus's, as a
                // compound's on a substrate - its edges' shifts added to its
                // Ec and Ev, the Z orbital's to the k·p's.
                const double exx = number(mf->text(QStringLiteral("exx")), Dim(), tr("%1's εxx").arg(where));
                const double ac = energy(mf->text(QStringLiteral("ac")), tr("%1's ac").arg(where)), av = energy(mf->text(QStringLiteral("av")), tr("%1's av").arg(where));
                const double bs = energy(mf->text(QStringLiteral("b")), tr("%1's b").arg(where));
                const double c11 = number(mf->text(QStringLiteral("C11")), Dim::of(-1, 1, -2), tr("%1's C11").arg(where));
                const double c12 = number(mf->text(QStringLiteral("C12")), Dim::of(-1, 1, -2), tr("%1's C12").arg(where));
                if (!(c11 > 0) || !std::isfinite(c12) || !std::isfinite(ac) || !std::isfinite(av) || !std::isfinite(bs))
                    problem(tr("%1: its strain wants ac, av, b, C11 and C12").arg(where));
                else {
                    b.ac = ac;
                    b.av = av;
                    b.bDef = bs;
                    b.c11 = c11;
                    b.c12 = c12;
                    const double ezz = -2 * c12 / c11 * exx, vol = 2 * exx + ezz, P = -av * vol, Qe = -bs * (exx - ezz);
                    const double D = std::isfinite(b.delta) ? b.delta : 0.3;
                    b.strain = exx;
                    b.dZ = 3 * Qe;
                    b.ecShift = ac * vol;
                    b.hhOver = -P - Qe;
                    b.lhOver = -P + 0.5 * (Qe - D + std::sqrt(D * D + 2 * D * Qe + 9 * Qe * Qe));
                }
            }
            if (mf->text(QStringLiteral("crystal")) == QLatin1String("wurtzite")) {
                WurtziteParameters w;
                auto six = [&](const QString& key, double* out, bool need) {
                    const QString t = mf->text(key).trimmed();
                    if (t.isEmpty()) {
                        if (need) problem(tr("%1: its wurtzite material wants %2 (six numbers)").arg(where, key));
                        return !need;
                    }
                    const QStringList parts = t.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
                    bool ok = parts.size() == 6;
                    for (int i = 0; ok && i < 6; ++i) out[i] = parts[i].toDouble(&ok);
                    if (!ok) problem(tr("%1: its %2 must be six numbers").arg(where, key));
                    return ok;
                };
                auto given = [&](const QString& key, double otherwise, const Dim& dim, bool eV) {
                    const QString t = mf->text(key).trimmed();
                    if (t.isEmpty()) return otherwise;
                    return eV ? energy(t, tr("%1's %2").arg(where, key)) : number(t, dim, tr("%1's %2").arg(where, key));
                };
                const Dim pascal = Dim::of(-1, 1, -2), polarization = Dim::of(-2, 0, 1, 1);
                if (!six(QStringLiteral("A"), w.A, true) || !six(QStringLiteral("D"), w.D, false)) continue;
                w.dcr = given(QStringLiteral("Dcr"), 0, Dim(), true);
                w.dso = given(QStringLiteral("Dso"), 0, Dim(), true);
                w.mez = b.me;
                w.mep = given(QStringLiteral("mep"), b.me, Dim(), false);
                w.a1 = given(QStringLiteral("a1"), 0, Dim(), true);
                w.a2 = given(QStringLiteral("a2"), 0, Dim(), true);
                w.C13 = given(QStringLiteral("C13"), NaN, pascal, false);
                w.C33 = given(QStringLiteral("C33"), NaN, pascal, false);
                w.Psp = given(QStringLiteral("Psp"), 0, polarization, false);
                w.e31 = given(QStringLiteral("e31"), 0, polarization, false);
                w.e33 = given(QStringLiteral("e33"), 0, polarization, false);
                b.meP = w.mep;
                b.EpZ = given(QStringLiteral("Epz"), b.Ep, Dim(), true);
                wzExx = given(QStringLiteral("exx"), 0, Dim(), false);
                const QString ezz = mf->text(QStringLiteral("ezz")).trimmed();
                if (!ezz.isEmpty()) wzEzz = number(ezz, Dim(), tr("%1's εzz").arg(where));
                else if (wzExx != 0) {
                    if (w.C33 > 0 && std::isfinite(w.C13)) wzEzz = -2 * w.C13 / w.C33 * wzExx;
                    else problem(tr("%1: its strain along c wants εzz, or C13 and C33").arg(where));
                }
                wzLibrary = false;
                wz = w;
            }
        } else {
            if (!mat) {
                problem(tr("%1 has no material").arg(where));
                continue;
            }
            auto text = [&](const QString& key) { return materialProperty(*mat, key, &constants); };
            const QString compound = materialProperty(*mat, QStringLiteral("compound")).trimmed();
            if (!compound.isEmpty() && isWurtzite(compound)) {
                // A nitride: its record (a gap given its 300 K one, moved with
                // the temperature as its compound's), pseudomorphic on the
                // substrate's lattice in the c-plane (εzz = -2 C13/C33 εxx).
                const double x = evaluateConstant(materialProperty(*mat, QStringLiteral("x")), constants, Dim()).value_or(0);
                const double y = evaluateConstant(materialProperty(*mat, QStringLiteral("y")), constants, Dim()).value_or(0);
                wz = wurtziteParameters(compound, x, a_T, &why, y);
                if (!wz) {
                    problem(tr("%1: %2").arg(where, why));
                    continue;
                }
                const QString eg = givenProperty(*mat, QStringLiteral("Eg"), &constants).trimmed();
                if (!eg.isEmpty()) {
                    // (A gap of T as it is at T; a number moved as the compound's.)
                    const double v = energy(eg, tr("%1's Eg").arg(where));
                    const std::optional<double> at300 = evaluateConstant(eg, materialsAt300, Dim());
                    const double v300 = at300 ? (Expression::compile(eg, materialsAt300).dim() == Dim::of(2, 1, -2) ? *at300 / Q : *at300) : v;
                    wz->Eg = std::abs(v - v300) > 1e-12 ? v : v + (wz->Eg - wurtziteParameters(compound, x, 300, nullptr, y)->Eg);
                }
                const QString vbo = givenProperty(*mat, QStringLiteral("VBO"), &constants).trimmed();
                if (!vbo.isEmpty()) wz->Ev = energy(vbo, tr("%1's VBO").arg(where));
                if (std::isfinite(substrate)) {
                    wzExx = (substrate - wz->a) / wz->a;
                    wzEzz = -2 * wz->C13 / wz->C33 * wzExx;
                }
            }
            std::optional<BandParameters> p;
            double shift300 = 0;   // its gap at T less its gap at 300 K (Varshni's)
            if (!compound.isEmpty()) {
                const double x = evaluateConstant(materialProperty(*mat, QStringLiteral("x")), constants, Dim()).value_or(0);
                const double y = evaluateConstant(materialProperty(*mat, QStringLiteral("y")), constants, Dim()).value_or(0);
                const BandSet set = bandSetOf(materialProperty(*mat, QStringLiteral("bands")));
                p = bandParameters(compound, x, y, a_T, &why, set);
                if (!p) {
                    problem(tr("%1: %2").arg(where, why));
                    continue;
                }
                shift300 = p->Eg - bandParameters(compound, x, y, 300, nullptr, set)->Eg;
            }
            // A property given on the material - or on its library entry, as
            // every physics reads a material (InP's measured 1.344 eV, not
            // its compound's 1.353) - over its compound's. A gap given is
            // the gap at 300 K: with a compound, moved with the temperature
            // as the compound's is.
            auto value = [&](const QString& key, double fromCompound, const Dim& dim, bool eV) {
                const QString given = givenProperty(*mat, key, &constants).trimmed();
                if (!given.isEmpty() || !p) {
                    const QString t = given.isEmpty() ? text(key) : given;
                    if (t.trimmed().isEmpty()) return NaN;
                    const double v = eV ? energy(t, tr("%1's %2").arg(where, key)) : number(t, dim, tr("%1's %2").arg(where, key));
                    if (key != QLatin1String("Eg") || given.isEmpty()) return v;
                    // A gap of T (the library's) as it is at T; a number, its
                    // 300 K one, moved as the compound's.
                    const std::optional<double> at300 = evaluateConstant(t, materialsAt300, Dim());
                    const double v300 = at300 ? (Expression::compile(t, materialsAt300).dim() == Dim::of(2, 1, -2) ? *at300 / Q : *at300) : v;
                    return std::abs(v - v300) > 1e-12 ? v : v + shift300;
                }
                return fromCompound;
            };
            const double Eg = value(QStringLiteral("Eg"), p ? p->Eg : NaN, Dim(), true);
            double VBO = value(QStringLiteral("VBO"), p ? p->VBO : NaN, Dim(), true);
            if (!std::isfinite(VBO)) {
                // The affinity's rule: Ev = -χ - Eg.
                const QString chi = text(QStringLiteral("chi"));
                if (!chi.trimmed().isEmpty() && std::isfinite(Eg)) VBO = -energy(chi, where) - Eg;
            }
            if (!std::isfinite(Eg) || !std::isfinite(VBO)) {
                problem(tr("%1: its material has no band gap and valence band offset (Eg with VBO or χ): give its compound, or "
                           "its bands as a Quantum Well Material of its own")
                            .arg(where));
                continue;
            }
            b.me = value(QStringLiteral("me"), p ? p->me : NaN, Dim(), false);
            const double g1 = value(QStringLiteral("gamma1"), p ? p->gamma1 : NaN, Dim(), false);
            const double g2 = value(QStringLiteral("gamma2"), p ? p->gamma2 : NaN, Dim(), false);
            if (!std::isfinite(b.me) || !std::isfinite(g1) || !std::isfinite(g2) || !(g1 > 2 * g2)) {
                problem(tr("%1: its material has no effective masses (me, and Luttinger's γ1 > 2 γ2)").arg(where));
                continue;
            }
            b.mhhZ = 1 / (g1 - 2 * g2);
            b.mlhZ = 1 / (g1 + 2 * g2);
            b.mhhP = 1 / (g1 + g2);
            b.mlhP = 1 / (g1 - g2);
            b.g1 = g1;
            b.g2 = g2;
            b.g3 = value(QStringLiteral("gamma3"), p ? p->gamma3 : NaN, Dim(), false);
            if (!std::isfinite(b.g3)) b.g3 = g2;
            b.delta = value(QStringLiteral("Delta"), p ? p->Delta : NaN, Dim(), true);
            b.Ep = value(QStringLiteral("Ep"), p ? p->Ep : NaN, Dim(), true);
            if (!std::isfinite(b.Ep)) b.Ep = 20;
            const QString nText = text(QStringLiteral("n"));
            b.nr = nText.trimmed().isEmpty() ? (p ? p->n : NaN) : evaluateConstant(nText, constants, Dim()).value_or(p ? p->n : NaN);
            const QString epsText = text(QStringLiteral("epsilonr"));
            b.epsr = epsText.trimmed().isEmpty() ? (p ? p->epsr : NaN) : evaluateConstant(epsText, constants, Dim()).value_or(p ? p->epsr : NaN);
            b.Ec = VBO + Eg;
            b.Ehh = b.Elh = VBO;
            b.Ec0 = b.Ec;
            b.Ev0 = VBO;
            if (std::isfinite(substrate)) {
                // Pseudomorphic on the substrate: biaxial strain in the plane
                // (Bir and Pikus; Chuang's signs).
                const double a0 = value(QStringLiteral("a0"), p ? p->a0 : NaN, Dim::length(), false);
                const double ac = value(QStringLiteral("ac"), p ? p->ac : NaN, Dim(), true);
                const double av = value(QStringLiteral("av"), p ? p->av : NaN, Dim(), true);
                const double bs = value(QStringLiteral("b"), p ? p->b : NaN, Dim(), true);
                const double c11 = value(QStringLiteral("C11"), p ? p->C11 : NaN, Dim::of(-1, 1, -2), false);
                const double c12 = value(QStringLiteral("C12"), p ? p->C12 : NaN, Dim::of(-1, 1, -2), false);
                const double delta = value(QStringLiteral("Delta"), p ? p->Delta : NaN, Dim(), true);
                if (!std::isfinite(a0) || !std::isfinite(ac) || !std::isfinite(av) || !std::isfinite(bs) || !(c11 > 0) || !std::isfinite(c12)) {
                    problem(tr("%1: its material has not what strain needs (a0, ac, av, b, C11, C12)").arg(where));
                    continue;
                }
                b.a0 = a0;
                b.ac = ac;
                b.av = av;
                b.bDef = bs;
                b.c11 = c11;
                b.c12 = c12;
                const double exx = (substrate - a0) / a0, ezz = -2 * c12 / c11 * exx;
                const double vol = 2 * exx + ezz;
                const double P = -av * vol, Qe = -bs * (exx - ezz);
                const double D = std::isfinite(delta) ? delta : 0.3;
                b.strain = exx;
                b.dZ = 3 * Qe;
                b.Ec += ac * vol;
                b.Ehh = VBO - P - Qe;
                b.Elh = VBO - P + 0.5 * (Qe - D + std::sqrt(D * D + 2 * D * Qe + 9 * Qe * Qe));
            } else if (p) {
                b.a0 = p->a0;
            }
        }
        if (wz) wurtzite(b, *wz, wzExx, wzEzz, wzLibrary);
        if (!(b.me > 0) || !(b.mhhZ > 0) || !(b.mlhZ > 0) || !(b.mhhP > 0) || !(b.mlhP > 0))
            problem(tr("%1: its effective masses must be more than 0").arg(where));
        // The electrons' valleys (Si, Ge): the feature's, else the material's.
        QString valleys = mf ? mf->text(QStringLiteral("valleys")).trimmed() : QString();
        if (valleys.isEmpty() && mat) valleys = materialProperty(*mat, QStringLiteral("valleys"), &constants).trimmed();
        if (const std::optional<std::vector<ValleySet>> sets = valleySets(valleys, &why)) b.valleys = *sets;
        else problem(tr("%1: its valleys: %2").arg(where, why));
    }
    {
        int wurtzites = 0, all = 0;
        for (const Band& b : a_bands)
            if (b.on) {
                ++all;
                wurtzites += b.wz ? 1 : 0;
            }
        if (wurtzites > 0 && wurtzites < all) problem(tr("%1: its layers are wurtzite and zinc blende both: one crystal").arg(a_node->name()));
        if (wurtzites > 0 && (a_valence == 4 || a_valence == 8))
            problem(tr("%1: the k·p 4x4 and 8x8 are zinc blende's: wurtzite's is the 6x6").arg(a_node->name()));
    }
    // An intermixed well (a process flow's Intermix): at each point the
    // edges of the alloy there, moved from its layer's as its composition's
    // move (unstrained; the masses the layer's).
    a_mix.reset();
    a_mixMaterial.assign(topo.domains.size(), -1);
    a_mixNominal.assign(topo.domains.size(), {0.0, 0.0});
    a_mixSet.assign(topo.domains.size(), BandSet::Vurgaftman);
    if (topo.process && topo.process->dopants) {
        const ProcessDopants& pd = *topo.process->dopants;
        bool mixed = false;
        for (const ProcessDopants::Field& f : pd.fields) mixed = mixed || f.name.startsWith(QLatin1String("x:"));
        for (int d = 0; mixed && d < int(topo.domains.size()); ++d) {
            const Band& b = a_bands[std::size_t(d)];
            const Node* mat = material[std::size_t(d)];
            if (!b.on || b.user || b.wz || !mat) continue;
            const QString compound = materialProperty(*mat, QStringLiteral("compound")).trimmed();
            if (compound.isEmpty()) continue;
            const double x = evaluateConstant(materialProperty(*mat, QStringLiteral("x")), constants, Dim()).value_or(0);
            const double y = evaluateConstant(materialProperty(*mat, QStringLiteral("y")), constants, Dim()).value_or(0);
            const BandSet set = bandSetOf(materialProperty(*mat, QStringLiteral("bands")));
            QString c;
            double cx = 0, cy = 0;
            const process::Sublattices nominal = process::sublatticesOf(compound, x, y);
            if (!nominal.ok || nominal.anion.empty() || !process::compoundOf(nominal, &c, &cx, &cy)) continue;
            const std::optional<BandParameters> p = bandParameters(c, cx, cy, a_T, nullptr, set);
            const int m = pd.materialAt(topo.domains[std::size_t(d)].inside.x(), topo.domains[std::size_t(d)].inside.y());
            if (!p || m < 0) continue;
            a_mixMaterial[std::size_t(d)] = m;
            a_mixNominal[std::size_t(d)] = {p->VBO + p->Eg, p->VBO};
            a_mixSet[std::size_t(d)] = set;
            a_mix = topo.process->dopants;
            a_mixScale = topo.unitScale;
        }
    }
    a_sets = 0;
    for (const Band& b : a_bands) a_sets = std::max(a_sets, int(b.valleys.size()));
    if (a_sets > 0 && !a_1d) problem(tr("%1: the electrons' valleys are a well's (1D): a wire's electrons one valley").arg(a_node->name()));
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    return true;
}

void QuantumProblem::initialize()
{
    Field& f = field();
    f.values.assign(std::size_t(f.space->size()) * std::size_t(f.count()), 0.0);
    f.extra.clear();
}

bool QuantumProblem::buildLine(QString* error)
{
    Line line;
    QString why;
    if (!detail::buildLine(a_s, field().domains, a_axis, a_node->text(QStringLiteral("at")), a_h1d, 0, &line, &why)) {
        *error = tr("%1: %2").arg(a_node->name(), why);
        return false;
    }
    a_S = line.S;
    a_E = line.E;
    a_L = line.L;
    a_z = line.z;
    a_elemDomain = line.domain;
    // A node is free where it is inside the physics' run of elements.
    const int nn = int(a_z.size()), ne = int(a_elemDomain.size());
    std::vector<int> touching(std::size_t(nn), 0);
    for (int e = 0; e < ne; ++e)
        if (a_elemDomain[std::size_t(e)] >= 0) {
            ++touching[std::size_t(2 * e)];
            touching[std::size_t(2 * e + 1)] += 2;
            ++touching[std::size_t(2 * e + 2)];
        }
    a_eq.assign(std::size_t(nn), -1);
    a_neq = 0;
    for (int i = 0; i < nn; ++i)
        if (touching[std::size_t(i)] >= 2) a_eq[std::size_t(i)] = a_neq++;
    if (a_neq == 0) {
        *error = tr("%1: its line crosses none of its domains").arg(a_node->name());
        return false;
    }
    // (Periodic: its run's two end nodes one equation - ψ the same there,
    // Bloch's at kz 0.)
    if (a_periodic) {
        int first = -1, last = -1;
        for (int i = 0; i < nn; ++i)
            if (touching[std::size_t(i)] >= 1) {
                if (first < 0) first = i;
                last = i;
            }
        if (first >= 0 && last > first && a_eq[std::size_t(first)] < 0 && a_eq[std::size_t(last)] < 0)
            a_eq[std::size_t(first)] = a_eq[std::size_t(last)] = a_neq++;
    }
    return true;
}

std::vector<double> QuantumProblem::polarizationEnergy(const std::vector<double>* rho, std::vector<double>* fields) const
{
    std::vector<double> out(a_points.size(), 0.0);
    // The physics' elements along the axis (its line may run either way):
    // each one's span, polarization, permittivity and free charge (C/m²).
    struct Element {
        double lo = 0, hi = 0, P = 0, eps = 1, sigma = 0, D = 0, F = 0, U = 0;
        int d = -1;
    };
    const QPointF u = (a_E - a_S) / a_L;
    auto axisOf = [&](double z) {
        const QPointF p = a_S + u * z;
        return a_axis == 1 ? p.y() : p.x();
    };
    std::vector<Element> els;
    std::vector<int> index(a_elemDomain.size(), -1);
    for (std::size_t e = 0; e < a_elemDomain.size(); ++e) {
        const int d = a_elemDomain[e];
        if (d < 0 || !a_bands[std::size_t(d)].on) continue;
        const Band& b = a_bands[std::size_t(d)];
        Element el;
        const double a0 = axisOf(a_z[2 * e]), a2 = axisOf(a_z[2 * e + 2]);
        el.lo = std::min(a0, a2);
        el.hi = std::max(a0, a2);
        el.P = a_polarity * b.P;
        el.eps = b.epsr > 0 ? b.epsr : (b.nr > 0 ? b.nr * b.nr : 10);
        el.d = d;
        index[e] = int(els.size());
        els.push_back(el);
    }
    if (rho)
        for (std::size_t i = 0; i < a_points.size() && i < a_pointElement.size(); ++i) {
            const int k = index[std::size_t(a_pointElement[i])];
            if (k >= 0) els[std::size_t(k)].sigma += (*rho)[i] * a_points[i].w * 1e-9;
        }
    std::vector<std::size_t> order(els.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return els[a].lo < els[b].lo; });
    if (order.empty()) return out;
    // D at each element (its own free charge half before it), less D0.
    double q = 0, num = 0, den = 0;
    for (std::size_t i : order) {
        Element& el = els[i];
        el.D = q + el.sigma / 2;
        q += el.sigma;
        num += (el.D - el.P) * (el.hi - el.lo) / el.eps;
        den += (el.hi - el.lo) / el.eps;
    }
    const double D0 = a_polarization == 1 ? -num / den : els[order.front()].P - els[order.front()].D;
    double U = 0;
    for (std::size_t i : order) {
        Element& el = els[i];
        el.F = (el.D + D0 - el.P) / (Eps0 * el.eps);
        el.U = U;   // (at its start)
        U += el.F * (el.hi - el.lo);
    }
    // 0 at the middle.
    double at0 = 0;
    for (std::size_t i : order) {
        const Element& el = els[i];
        if (a_s0 >= el.lo && a_s0 <= el.hi) {
            at0 = el.U + el.F * (a_s0 - el.lo);
            break;
        }
        if (a_s0 > el.hi) at0 = el.U + el.F * (el.hi - el.lo);
    }
    for (std::size_t i = 0; i < a_points.size() && i < a_pointElement.size(); ++i) {
        const int k = index[std::size_t(a_pointElement[i])];
        if (k < 0) continue;
        const Element& el = els[std::size_t(k)];
        out[i] = el.U + el.F * (a_points[i].a - el.lo) - at0;
    }
    if (fields) {
        fields->assign(a_bands.size(), 0.0);
        std::vector<double> length(a_bands.size(), 0.0);
        for (const Element& el : els) {
            (*fields)[std::size_t(el.d)] += el.F * (el.hi - el.lo);
            length[std::size_t(el.d)] += el.hi - el.lo;
        }
        for (std::size_t d = 0; d < fields->size(); ++d)
            if (length[d] > 0) (*fields)[d] /= length[d];
    }
    return out;
}

void QuantumProblem::bandsAt(PointEvaluator& ev, int d, double a, double out[Carriers]) const
{
    const Band& b = a_bands[std::size_t(d)];
    double ec = b.Ec, ehh = b.Ehh, elh = b.Elh;
    if (b.user) {
        auto eV = [&](const Expression& e) {
            const double v = ev.eval(e);
            return e.dim() == Dim::of(2, 1, -2) ? v / Q : v;
        };
        ec = eV(b.userEc) + b.ecShift;
        ehh = elh = eV(b.userEv);
        ehh += b.hhOver;
        elh += b.lhOver;
    }
    if (a_mix && a_mixMaterial[std::size_t(d)] >= 0) {
        double dEc = 0, dEv = 0;
        if (mixShift(ev.point() / a_mixScale, d, &dEc, &dEv)) {
            ec += dEc;
            ehh += dEv;
            elh += dEv;
        }
    }
    double U = a_F * (a - a_s0);
    if (a_V.isValid()) {
        const double v = ev.eval(a_V);
        if (std::isfinite(v)) U -= v;
    }
    out[0] = ec + U;
    out[1] = ehh + U;
    out[2] = elh + U;
}

bool QuantumProblem::mixShift(QPointF p, int d, double* dEc, double* dEv) const
{
    const int m = a_mixMaterial[std::size_t(d)];
    process::Sublattices s;
    s.ok = true;
    for (const ProcessDopants::Field& f : a_mix->fields) {
        if (!f.name.startsWith(QLatin1String("x:"))) continue;
        const process::Element* e = process::element(f.name.mid(2));
        if (!e) continue;
        const double v = a_mix->at(f, p.x(), p.y(), m, false);
        if (!std::isfinite(v) || v <= 0) continue;
        // (Group III on the cations' sublattice, group V on the anions'.)
        const bool cation = e->Z == 5 || e->Z == 13 || e->Z == 31 || e->Z == 49;
        (cation ? s.cation : s.anion).emplace_back(e->Z, v);
    }
    QString c;
    double x = 0, y = 0;
    if (s.cation.empty() || s.anion.empty() || !process::compoundOf(s, &c, &x, &y)) return false;
    const std::optional<BandParameters> b = bandParameters(c, x, y, a_T, nullptr, a_mixSet[std::size_t(d)]);
    if (!b) return false;
    *dEc = b->VBO + b->Eg - a_mixNominal[std::size_t(d)][0];
    *dEv = b->VBO - a_mixNominal[std::size_t(d)][1];
    return true;
}

bool QuantumProblem::prepare(QString* error)
{
    Field& f = field();
    const Mesh& mesh = a_s.mesh();
    const Topology& topo = a_s.topology();
    const Space& sp = *f.space;
    PointEvaluator ev(a_s);
    a_points.clear();
    a_pointElement.clear();
    a_Upol.clear();
    // The middle of the physics along the axis: the field's potential 0 there.
    {
        double lo = Inf(), hi = -Inf();
        for (std::size_t d = 0; d < topo.domains.size(); ++d)
            if (f.domains[d]) {
                const QRectF& r = topo.domains[d].bounds;
                lo = std::min(lo, (a_axis == 1 ? r.top() : r.left()) * mesh.unitScale);
                hi = std::max(hi, (a_axis == 1 ? r.bottom() : r.right()) * mesh.unitScale);
            }
        a_s0 = (lo + hi) / 2;
    }
    auto axisOf = [&](QPointF p) { return a_axis == 1 ? p.y() : p.x(); };
    if (a_periodic && a_F != 0) {
        *error = tr("%1: a periodic line takes no field along its axis - its potential would drop across each period, the states not "
                    "Bloch's (a superlattice in a field: its periods drawn, hard walls)")
                     .arg(a_node->name());
        return false;
    }
    if (a_1d) {
        if (!buildLine(error)) return false;
        const Locator locator(mesh);
        const QPointF u = (a_E - a_S) / a_L;
        const auto& rule = lineRule(4);
        for (std::size_t e = 0; e < a_elemDomain.size(); ++e) {
            const int d = a_elemDomain[e];
            if (d < 0) continue;
            const double z0 = a_z[2 * e], z2 = a_z[2 * e + 2], h = z2 - z0, hnm = h * 1e9;
            for (const auto& q : rule) {
                const double xi = q[0];
                Point pt;
                pt.d = d;
                pt.w = q[1] * hnm;
                pt.n = 3;
                const double N[3] = {(1 - xi) * (1 - 2 * xi), 4 * xi * (1 - xi), xi * (2 * xi - 1)};
                const double dN[3] = {4 * xi - 3, 4 - 8 * xi, 4 * xi - 1};
                for (int i = 0; i < 3; ++i) {
                    pt.eq[i] = a_eq[2 * e + std::size_t(i)];
                    pt.N[i] = N[i];
                    pt.ga[i] = dN[i] / hnm;
                    pt.gc[i] = 0;
                }
                const QPointF P = a_S + u * (z0 + xi * h);
                pt.a = axisOf(P);
                // (On a mesh edge it may be in neither triangle by a rounding:
                // nudged across the line.)
                int t = -1;
                double rx = 0, ry = 0;
                bool located = false;
                const QPointF across(-u.y() * 1e-6 * a_L, u.x() * 1e-6 * a_L);
                for (const QPointF& p : {P, P + across, P - across})
                    if (!located && locator.locate(p / mesh.unitScale, &t, &rx, &ry)) located = true;
                double b[Carriers];
                if (located) {
                    ev.moveTo(t, rx, ry);
                    bandsAt(ev, d, pt.a, b);
                    pt.t = t;
                    pt.rx = rx;
                    pt.ry = ry;
                } else {
                    // Nowhere to evaluate an expression: the bands' constants and the field.
                    const Band& band = a_bands[std::size_t(d)];
                    const double U = a_F * (pt.a - a_s0);
                    b[0] = band.user ? NaN : band.Ec + U;
                    b[1] = band.user ? NaN : band.Ehh + U;
                    b[2] = band.user ? NaN : band.Elh + U;
                }
                pt.pot[0] = b[0];
                pt.pot[1] = -b[1];
                pt.pot[2] = -b[2];
                a_points.push_back(pt);
                a_pointElement.push_back(int(e));
            }
        }
        // The polarization's field: the bands moved by its potential.
        a_Upol.assign(a_points.size(), 0.0);
        if (a_polarization) {
            std::vector<double> fields;
            a_Upol = polarizationEnergy(a_rho.size() == a_points.size() ? &a_rho : nullptr, &fields);
            QStringList said;
            for (std::size_t d = 0; d < fields.size(); ++d)
                if (a_bands[d].on)
                    said << tr("%1 %2").arg(topo.objectNames.value(topo.domains[d].owner), formatQuantity(fields[d], Dim::of(1, 1, -3, -1)));
            if (!a_screening)
                a_s.log << tr("%1: the polarization's field (%2%3; along the axis): %4")
                               .arg(a_node->name(), a_polarization == 1 ? tr("periodic") : tr("none at its ends"),
                                    a_rho.size() == a_points.size() ? tr(", its carriers' screening") : QString(), said.join(QStringLiteral(", ")));
            for (std::size_t i = 0; i < a_points.size(); ++i) {
                a_points[i].pot[0] += a_Upol[i];
                a_points[i].pot[1] -= a_Upol[i];
                a_points[i].pot[2] -= a_Upol[i];
            }
        }
    } else {
        // The degrees of freedom of its triangles; those where the physics
        // ends (but where its wavefunction is free) fixed at 0.
        const int nd = sp.size();
        std::vector<char> on(std::size_t(nd), 0), fixed(std::size_t(nd), 0);
        for (int t = 0; t < sp.elements(); ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            for (int i = 0; i < sp.perElement(); ++i) on[std::size_t(sp.dofs(t)[i])] = 1;
        }
        auto inside = [&](int t) { return t >= 0 && mesh.domain[std::size_t(t)] >= 0 && f.domains[std::size_t(mesh.domain[std::size_t(t)])]; };
        for (std::size_t e = 0; e < mesh.edges.size(); ++e) {
            const Mesh::Edge& me = mesh.edges[e];
            if (inside(me.left) == inside(me.right)) continue;
            if (std::find(a_flux.begin(), a_flux.end(), me.boundary) != a_flux.end()) continue;
            for (int dof : sp.edgeDofs(int(e)))
                if (dof >= 0) fixed[std::size_t(dof)] = 1;
        }
        a_eq.assign(std::size_t(nd), -1);
        a_neq = 0;
        for (int i = 0; i < nd; ++i)
            if (on[std::size_t(i)] && !fixed[std::size_t(i)]) a_eq[std::size_t(i)] = a_neq++;
        if (a_neq == 0) {
            *error = tr("%1: no degree of freedom is free").arg(a_node->name());
            return false;
        }
        const TriangleRule& rule = triangleRule(a_order == 1 ? 3 : 5);
        const bool axi = a_s.axisymmetric();
        for (int t = 0; t < sp.elements(); ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            for (const auto& q : rule.points) {
                const Space::Map m = sp.map(t, q[0], q[1]);
                Shape sh;
                shapeAt(sp.order(), q[0], q[1], sh);
                Point pt;
                pt.d = d;
                pt.w = q[2] * std::abs(m.det) * 1e18 * (axi ? m.x.x() * 1e9 : 1.0);
                pt.a = axisOf(m.x);
                pt.n = sh.count;
                for (int i = 0; i < sh.count; ++i) {
                    double gx = 0, gy = 0;
                    Space::gradient(m, sh.dxi[i], sh.deta[i], gx, gy);
                    pt.eq[i] = a_eq[std::size_t(sp.dofs(t)[i])];
                    pt.N[i] = sh.n[i];
                    pt.ga[i] = (a_axis == 1 ? gy : gx) * 1e-9;
                    pt.gc[i] = (a_axis == 1 ? gx : gy) * 1e-9;
                }
                ev.moveTo(t, q[0], q[1]);
                double b[Carriers];
                bandsAt(ev, d, pt.a, b);
                pt.pot[0] = b[0];
                pt.pot[1] = -b[1];
                pt.pot[2] = -b[2];
                a_points.push_back(pt);
            }
        }
    }
    // The bands kept with the field, for its variables: at each triangle's
    // corners (each its own: the offsets stay sharp), the masses by domain.
    const int nt = int(mesh.triangles.size()), ndom = int(topo.domains.size());
    auto fixedData = std::make_shared<std::vector<double>>();
    std::vector<double>& fx = *fixedData;
    fx.assign(std::size_t(Header) + a_z.size() + std::size_t(9 * nt + 4 * ndom), NaN);
    fx[0] = a_1d ? 1 : 2;
    fx[1] = a_axis;
    fx[2] = a_S.x();
    fx[3] = a_S.y();
    fx[4] = a_E.x();
    fx[5] = a_E.y();
    fx[6] = double(a_z.size());
    fx[7] = a_drawn * Q;
    fx[8] = nt;
    fx[9] = ndom;
    std::copy(a_z.begin(), a_z.end(), fx.begin() + Header);
    const std::size_t triBase = std::size_t(Header) + a_z.size(), domBase = triBase + std::size_t(9 * nt);
    static const double corner[3][2] = {{0, 0}, {1, 0}, {0, 1}};
    for (int t = 0; t < nt; ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)] || !a_bands[std::size_t(d)].on) continue;
        for (int k = 0; k < 3; ++k) {
            ev.moveTo(t, corner[k][0], corner[k][1]);
            double b[Carriers];
            bandsAt(ev, d, axisOf(ev.point()), b);
            for (int c = 0; c < Carriers; ++c) fx[triBase + std::size_t(9 * t + 3 * c + k)] = b[c] * Q;
        }
    }
    for (int d = 0; d < ndom; ++d) {
        const Band& b = a_bands[std::size_t(d)];
        if (!b.on) continue;
        fx[domBase + std::size_t(4 * d)] = b.me;
        fx[domBase + std::size_t(4 * d + 1)] = b.mhhZ;
        fx[domBase + std::size_t(4 * d + 2)] = b.mlhZ;
        fx[domBase + std::size_t(4 * d + 3)] = b.strain;
    }
    f.fixed = fixedData;
    // Where the physics ends: the band edges past which a state is not bound.
    for (int c = 0; c < Carriers; ++c) a_barrier[c] = c == 0 ? Inf() : -Inf();
    if (a_1d) {
        // The first and last points of each run of elements.
        const auto& rule = lineRule(4);
        const std::size_t per = rule.size();
        std::size_t p = 0;
        for (std::size_t e = 0; e < a_elemDomain.size(); ++e) {
            if (a_elemDomain[e] < 0) continue;
            const bool startsRun = e == 0 || a_elemDomain[e - 1] < 0;
            const bool endsRun = e + 1 == a_elemDomain.size() || a_elemDomain[e + 1] < 0;
            for (const std::size_t k : {std::size_t(0), per - 1}) {
                if ((k == 0 && !startsRun) || (k == per - 1 && !endsRun)) continue;
                const Point& pt = a_points[p + k];
                a_barrier[0] = std::min(a_barrier[0], pt.pot[0]);
                a_barrier[1] = std::max(a_barrier[1], -pt.pot[1]);
                a_barrier[2] = std::max(a_barrier[2], -pt.pot[2]);
            }
            p += per;
        }
    } else {
        auto inside = [&](int t) { return t >= 0 && mesh.domain[std::size_t(t)] >= 0 && f.domains[std::size_t(mesh.domain[std::size_t(t)])]; };
        for (const Mesh::Edge& me : mesh.edges) {
            if (inside(me.left) == inside(me.right)) continue;
            const int t = inside(me.left) ? me.left : me.right;
            const auto& tri = mesh.triangles[std::size_t(t)];
            for (int k = 0; k < 3; ++k) {
                if (tri[std::size_t(k)] != me.a && tri[std::size_t(k)] != me.b) continue;
                const double ec = fx[triBase + std::size_t(9 * t + k)] / Q;
                const double eh = fx[triBase + std::size_t(9 * t + 3 + k)] / Q, el = fx[triBase + std::size_t(9 * t + 6 + k)] / Q;
                a_barrier[0] = std::min(a_barrier[0], ec);
                a_barrier[1] = std::max(a_barrier[1], eh);
                a_barrier[2] = std::max(a_barrier[2], el);
            }
        }
    }
    // The wells: the domains of the smallest gap.
    std::vector<double> gapSum(std::size_t(ndom), 0), wSum(std::size_t(ndom), 0);
    for (const Point& pt : a_points) {
        gapSum[std::size_t(pt.d)] += pt.w * (pt.pot[0] + std::min(pt.pot[1], pt.pot[2]));
        wSum[std::size_t(pt.d)] += pt.w;
    }
    double least = Inf();
    for (int d = 0; d < ndom; ++d)
        if (wSum[std::size_t(d)] > 0) least = std::min(least, gapSum[std::size_t(d)] / wSum[std::size_t(d)]);
    a_active.assign(std::size_t(ndom), 0);
    double size = 0;
    for (int d = 0; d < ndom; ++d)
        if (wSum[std::size_t(d)] > 0 && gapSum[std::size_t(d)] / wSum[std::size_t(d)] < least + 1e-3) {
            a_active[std::size_t(d)] = 1;
            size += wSum[std::size_t(d)];
        }
    a_activeSize = size * (a_1d ? 1e-9 : 1e-18);
    return true;
}

double QuantumProblem::integral(const Eigen::VectorXd& a, const Eigen::VectorXd& b, const std::function<double(int, double)>& f) const
{
    double sum = 0;
    for (const Point& pt : a_points) {
        double va = 0, vb = 0;
        for (int i = 0; i < pt.n; ++i)
            if (pt.eq[i] >= 0) {
                va += pt.N[i] * a(pt.eq[i]);
                vb += pt.N[i] * b(pt.eq[i]);
            }
        sum += pt.w * va * vb * (f ? f(pt.d, pt.a) : 1.0);
    }
    return sum;
}

bool QuantumProblem::states(int c, int count, QString* error)
{
    // The electrons' valley sets (Si, Ge) each a ladder of its own - its
    // mass along the axis, its density of states' in the plane, its edge -,
    // merged by energy.
    if (c != 0 || a_sets == 0) return ladder(c, count, error);
    std::vector<State> all;
    for (int v = 0; v < a_sets; ++v) {
        a_set = v;
        const bool ok = ladder(c, count, error);
        a_set = -1;
        if (!ok) return false;
        for (State& st : a_states[0]) {
            st.valley = v;
            all.push_back(std::move(st));
        }
    }
    std::sort(all.begin(), all.end(), [](const State& a, const State& b) { return a.E < b.E; });
    a_states[0] = std::move(all);
    return true;
}

bool QuantumProblem::ladder(int c, int count, QString* error)
{
    a_states[c].clear();
    if (!a_on[c]) return true;
    const int n = a_neq;
    std::vector<Eigen::Triplet<double>> tk, tm;
    tk.reserve(a_points.size() * 36);
    tm.reserve(a_points.size() * 36);
    double lowest = Inf();
    for (const Point& pt : a_points) {
        const double along = HB2M / mass(c, pt.d, true), across = HB2M / mass(c, pt.d, false);
        const double pot = pt.pot[c] + offset(c, pt.d);
        lowest = std::min(lowest, pot);
        for (int i = 0; i < pt.n; ++i) {
            if (pt.eq[i] < 0) continue;
            for (int j = 0; j < pt.n; ++j) {
                if (pt.eq[j] < 0) continue;
                const double m = pt.w * pt.N[i] * pt.N[j];
                tk.emplace_back(pt.eq[i], pt.eq[j], pt.w * (along * pt.ga[i] * pt.ga[j] + across * pt.gc[i] * pt.gc[j]) + pot * m);
                tm.emplace_back(pt.eq[i], pt.eq[j], m);
            }
        }
    }
    Eigen::SparseMatrix<double> K(n, n), M(n, n);
    K.setFromTriplets(tk.begin(), tk.end());
    M.setFromTriplets(tm.begin(), tm.end());
    if (!std::isfinite(lowest)) {
        *error = tr("%1: its bands are not finite (a potential not defined where it is solved?)").arg(a_node->name());
        return false;
    }
    count = std::min(count, n);
    std::vector<std::pair<double, Eigen::VectorXd>> found;
    if (n <= 600) {
        const Eigen::MatrixXd Kd(K), Md(M);
        Eigen::GeneralizedSelfAdjointEigenSolver<Eigen::MatrixXd> es(Kd, Md);
        if (es.info() != Eigen::Success) {
            *error = tr("%1: the %2 states' eigenproblem was not solved").arg(a_node->name(), QLatin1String(CarrierName[c]));
            return false;
        }
        for (int k = 0; k < count; ++k) found.emplace_back(es.eigenvalues()(k), es.eigenvectors().col(k));
    } else {
        // Shift and invert below the lowest potential: (K - σM) is positive
        // definite, its inverse's largest eigenvalues the lowest states.
        const double sigma = lowest - 0.01 - 1e-3 * std::abs(lowest);
        Eigen::SparseMatrix<double> A = K - sigma * M;
        A.makeCompressed();
        Eigen::SimplicialLLT<Eigen::SparseMatrix<double>> llt(A);
        if (llt.info() != Eigen::Success) {
            *error = tr("%1: the shifted %2 matrix is not positive definite").arg(a_node->name(), QLatin1String(CarrierName[c]));
            return false;
        }
        const int asked = std::min(count + 3, n - 2);
        const EigenResult r = largestEigenvalues(
            n, asked,
            [&](const Eigen::VectorXcd& x, Eigen::VectorXcd& y) {
                const Eigen::VectorXd re = M * x.real(), im = M * x.imag();
                const Eigen::VectorXd sr = llt.solve(re), si = llt.solve(im);
                y.resize(n);
                for (int i = 0; i < n; ++i) y(i) = std::complex<double>(sr(i), si(i));
            },
            0, 1e-10, 300, [this] { return cancelled(); });
        if (r.cancelled) {
            *error = tr("%1: cancelled").arg(a_node->name());
            return false;
        }
        if (!r.error.isEmpty()) a_s.log << tr("Warning: %1: %2").arg(a_node->name(), r.error);
        // Rayleigh and Ritz in the real space they span (a degenerate pair
        // comes out of the solver mixed, complex): M-orthonormal, then K's
        // eigenvectors there.
        std::vector<Eigen::VectorXd> basis;
        auto addVector = [&](Eigen::VectorXd v) {
            for (int pass = 0; pass < 2; ++pass)
                for (const Eigen::VectorXd& b : basis) v -= b * b.dot(M * v);
            const double norm = std::sqrt(std::max(0.0, v.dot(M * v)));
            if (norm > 1e-8) basis.push_back(v / norm);
        };
        for (const Eigen::VectorXcd& v : r.vectors) {
            const double scale = v.norm();
            if (!(scale > 0)) continue;
            addVector(v.real() / scale);
            addVector(v.imag() / scale);
        }
        if (basis.empty()) {
            *error = tr("%1: no %2 state was found").arg(a_node->name(), QLatin1String(CarrierName[c]));
            return false;
        }
        const int m = int(basis.size());
        Eigen::MatrixXd B(n, m);
        for (int k = 0; k < m; ++k) B.col(k) = basis[std::size_t(k)];
        const Eigen::MatrixXd small = B.transpose() * (K * B);
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(0.5 * (small + small.transpose()));
        for (int k = 0; k < std::min(count, m); ++k) found.emplace_back(es.eigenvalues()(k), B * es.eigenvectors().col(k));
    }
    for (auto& [lambda, x] : found) {
        State s;
        // (Normalized: xᵀ M x = 1; its largest value positive.)
        const double norm = std::sqrt(std::max(1e-300, x.dot(M * x)));
        x /= norm;
        Eigen::Index big = 0;
        x.cwiseAbs().maxCoeff(&big);
        if (x(big) < 0) x = -x;
        s.E = c == 0 ? lambda : -lambda;
        s.x = x;
        s.pos = integral(x, x, [](int, double a) { return a; });
        s.bound = c == 0 ? s.E < a_barrier[0] : s.E > a_barrier[c];
        const double invM = integral(x, x, [&](int d, double) { return 1 / mass(c, d, false); });
        s.mPar = invM > 0 ? 1 / invM : NaN;
        s.Ep = integral(x, x, [&](int d, double) { return a_bands[std::size_t(d)].Ep; });
        s.EpZ = integral(x, x, [&](int d, double) {
            const Band& b = a_bands[std::size_t(d)];
            return std::isfinite(b.EpZ) ? b.EpZ : b.Ep;
        });
        s.nr = integral(x, x, [&](int d, double) {
            const double v = a_bands[std::size_t(d)].nr;
            return std::isfinite(v) ? v : 3.5;
        });
        if (a_set >= 0) {
            // Its valleys: where they are given (a barrier of one valley, me's,
            // has none of its own), averaged over |ψ|² there.
            auto has = [&](int d) { return std::size_t(a_set) < a_bands[std::size_t(d)].valleys.size(); };
            const double in = integral(x, x, [&](int d, double) { return has(d) ? 1.0 : 0.0; });
            if (in > 1e-12) s.g = integral(x, x, [&](int d, double) { return has(d) ? a_bands[std::size_t(d)].valleys[std::size_t(a_set)].g : 0.0; }) / in;
            else
                for (const Band& b : a_bands)
                    if (std::size_t(a_set) < b.valleys.size()) {
                        s.g = b.valleys[std::size_t(a_set)].g;
                        break;
                    }
        }
        a_states[c].push_back(std::move(s));
    }
    std::sort(a_states[c].begin(), a_states[c].end(), [c](const State& a, const State& b) { return c == 0 ? a.E < b.E : a.E > b.E; });
    if (!a_states[c].empty()) a_states[c].back().top = true;
    return true;
}

bool QuantumProblem::kpLevels(QString* said, QString* error)
{
    // At k = 0 the heavy holes are the effective mass's (none of the Z
    // orbital, coupled to nothing); the light holes take the split-off band
    // in (6x6) and the S band through Kane's P (8x8), as the 8x8's
    // electrons the valence bands - their energies the k·p's, each kind in
    // order (a pair's kind by its Z orbital's share: a heavy hole's none,
    // a light hole's 2/3, the split-off's 1/3), their envelopes the
    // effective mass's.
    const int pairs = int(a_states[1].size() + a_states[2].size());   // (and a split-off's few among them)
    const int electrons = a_valence == 8 ? int(a_states[0].size()) : 0;
    if (pairs == 0 && electrons == 0) return true;
    if (!kpBands(pairs + 8, 0, 1, error, electrons)) return false;   // (eight pairs more: each kind's found)
    const auto cleared = qScopeGuard([this] { a_kps.clear(); });
    if (a_kps.empty() || a_kps.front().E.empty()) return true;
    const KpBands& b = a_kps.front();
    const int n = a_neq;
    QStringList moved;
    if (electrons > 0 && !b.C.empty()) {
        int e = 0;
        for (std::size_t i = 0; i < a_states[0].size() && 2 * i + 1 < b.C[0].size(); ++i, ++e) a_states[0][i].E = 0.5 * (b.C[0][2 * i] + b.C[0][2 * i + 1]);
        moved << tr("%1 electrons").arg(e);
    }
    std::vector<double> kind[Carriers];   // (by carrier: hh 1, lh 2)
    for (std::size_t j = 0; j + 1 < b.E[0].size(); j += 2) {
        const Eigen::VectorXcd& x = b.x[0][j];
        double z = 0, all = 0;
        for (int c = 0; c < 6; ++c) {
            const double w = x.segment(c * n, n).squaredNorm();
            all += w;
            if (c == 2 || c == 5) z += w;
        }
        const double share = all > 0 ? z / all : 0;
        if (share < 0.1) kind[1].push_back(b.E[0][j]);
        else if (share > 0.5) kind[2].push_back(b.E[0][j]);   // (between: split-off's, none's)
    }
    for (int c = 1; c < Carriers; ++c) {
        std::size_t i = 0;
        for (; i < a_states[c].size() && i < kind[c].size(); ++i) a_states[c][i].E = kind[c][i];
        if (i > 0) moved << tr("%1 %2").arg(i).arg(QLatin1String(CarrierName[c]));
    }
    if (said && !moved.isEmpty())
        *said = tr("; the %1x%1 k·p's energies at k = 0 for %2 (their envelopes the effective mass's)").arg(a_valence).arg(moved.join(QStringLiteral(", ")));
    return true;
}

int QuantumProblem::solveModes(const SolveOptions& options, int count, double guess, QString* error)
{
    (void)options;
    (void)guess;
    if (!prepare(error)) return -1;
    int most = 0;
    for (int c = 0; c < Carriers; ++c) {
        if (!states(c, std::max(1, count), error)) return -1;
        most = std::max(most, int(a_states[c].size()));
    }
    // A States step under the 6x6 or 8x8: their levels at k = 0 (the check
    // of 21 October: kp8's electrons the effective mass's, not a word).
    QString kpSaid;
    if (a_inner == 0 && (a_valence == 6 || a_valence == 8) && !kpLevels(&kpSaid, error)) return -1;
    // The overlaps the globals give, and the direct transition.
    for (int c = 1; c < Carriers; ++c) {
        a_overlap[c].assign(std::min<std::size_t>(4, a_states[0].size()), {});
        for (std::size_t i = 0; i < a_overlap[c].size(); ++i)
            for (std::size_t j = 0; j < std::min<std::size_t>(4, a_states[c].size()); ++j) {
                const double ov = integral(a_states[0][i].x, a_states[c][j].x, {});
                a_overlap[c][i].push_back(ov * ov);
            }
    }
    a_directE = a_directO = NaN;
    {
        std::vector<std::array<double, 2>> pairs;   // energy (eV), overlap
        double best = 0;
        for (const State& e : a_states[0])
            for (const State& h : a_states[1]) {
                const double ov = integral(e.x, h.x, {});
                pairs.push_back({e.E - h.E, ov * ov});
                best = std::max(best, ov * ov);
            }
        for (const auto& p : pairs)
            if (best > 0 && p[1] >= best / 2 && !(p[0] >= a_directE)) {
                a_directE = p[0];
                a_directO = p[1];
            }
    }
    QStringList found;
    for (int c = 0; c < Carriers; ++c) {
        if (!a_on[c]) continue;
        int bound = 0;
        for (const State& s : a_states[c]) bound += s.bound ? 1 : 0;
        found << tr("%1 %2 (%3 bound%4)")
                     .arg(a_states[c].size())
                     .arg(QLatin1String(CarrierName[c]))
                     .arg(bound)
                     .arg(c == 0 && a_sets > 0 ? tr("; in %1 valley sets, each a ladder of its own").arg(a_sets) : QString());
    }
    a_s.log << tr("%1: %2 states, %3 degrees of freedom%4%5")
                   .arg(a_node->name(), found.join(QLatin1String(", ")))
                   .arg(a_neq)
                   .arg(a_1d ? tr(" along a line of %1").arg(formatQuantity(a_L, Dim::length())) : QString())
                   .arg(kpSaid);
    // A field past the gap's drop: the lowest electron state below the
    // highest hole's, each held by the line's ends alone - its spectrum was
    // computed from them and given, the transitions below zero, said nowhere
    // (the hunt of 22 October, A10).
    if (a_F != 0) {
        double lowest = Inf(), highest = -Inf();
        for (int c = 0; c < Carriers; ++c)
            if (a_on[c])
                for (const State& st : a_states[c]) {
                    if (c == 0) lowest = std::min(lowest, st.E);
                    else highest = std::max(highest, st.E);
                }
        if (std::isfinite(lowest) && std::isfinite(highest) && lowest <= highest)
            a_s.log << tr("Warning: %1: in its field, %2%3, the lowest electron state lies %4 below the highest hole state - the gap gone "
                          "across the line, each state held by its ends alone: not a well's states, nor what they absorb (a field in "
                          "kV/cm meant? 1 kV/cm is 0.1 MV/m)")
                           .arg(a_node->name(), formatQuantity(a_F, Dim::of(1, 1, -3, -1)))
                           .arg(a_1d && a_L > 0 ? tr(" (%1 over the line's %2)").arg(formatQuantity(std::abs(a_F) * a_L, Dim::voltage()),
                                                                                    formatQuantity(a_L, Dim::length()))
                                                : QString())
                           .arg(electronVolts((highest - lowest) * Q));
    }
    densities();
    field().solved = true;
    a_spec = Spectrum();
    a_specSelected = -1;
    return most;
}

void QuantumProblem::densities()
{
    const Space& sp = *field().space;
    const int nd = sp.size();
    for (auto& v : a_density) v.assign(std::size_t(nd), 0.0);
    for (int c = 0; c < Carriers; ++c) a_sheet[c].clear();
    a_sheetValley.clear();
    if (!a_etaN.isValid() && !a_etaP.isValid()) return;
    const Mesh& mesh = a_s.mesh();
    const Field& f = field();
    const double kT = KB * a_T, scale = densityScale();
    const QPointF u = a_1d && a_L > 0 ? (a_E - a_S) / a_L : QPointF();
    // A state's ψ at a degree of freedom (in the integration's units): a
    // line's where the point projects, or the mesh's own.
    auto psiAt = [&](const State& st, int dof) {
        if (!a_1d) return a_eq[std::size_t(dof)] >= 0 ? st.x(a_eq[std::size_t(dof)]) : 0.0;
        const QPointF p = sp.point(dof) - a_S;
        const double z = p.x() * u.x() + p.y() * u.y();
        if (z < 0 || z > a_L || a_elemDomain.empty()) return 0.0;
        const auto it = std::upper_bound(a_z.begin(), a_z.end(), z);
        std::size_t e = std::size_t(std::max<std::ptrdiff_t>(0, (it - a_z.begin()) - 1)) / 2;
        e = std::min(e, a_elemDomain.size() - 1);
        const double z0 = a_z[2 * e], z2 = a_z[2 * e + 2];
        const double xi = z2 > z0 ? (z - z0) / (z2 - z0) : 0;
        auto at = [&](std::size_t node) { return a_eq[node] >= 0 ? st.x(a_eq[node]) : 0.0; };
        return (1 - xi) * (1 - 2 * xi) * at(2 * e) + 4 * xi * (1 - xi) * at(2 * e + 1) + xi * (2 * xi - 1) * at(2 * e + 2);
    };
    // Each state's carriers per |ψ|² (1/m³ for |ψ|² in SI) at η = (the Fermi
    // level - the state) / kT: a well's states free in the plane (m kT/πħ²
    // ln(1 + e^η), each valley's: g of them), a wire's along z (√(2 m kT/πħ²)
    // ℱ-1/2(η)), a dot's none (2 f(η)).
    auto filled = [&](double mPar, double eta) {
        if (a_1d) return mPar * M0 * kT / (M_PI * Hbar * Hbar) * softplus(eta);
        if (a_s.axisymmetric()) return 2 * fermi(-eta);
        return std::sqrt(2 * mPar * M0 * kT / (M_PI * Hbar * Hbar)) * fermiMinusHalf(eta);
    };
    std::vector<char> done(std::size_t(nd), 0);
    PointEvaluator ev(a_s);
    double highest[Carriers] = {0, 0, 0}, total[Carriers] = {0, 0, 0};
    for (int t = 0; t < sp.elements(); ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)] || !a_bands[std::size_t(d)].on) continue;
        const int* dof = sp.dofs(t);
        const auto& g = sp.nodes(t);
        const QPointF e1 = g[1] - g[0], e2 = g[2] - g[0];
        const double det = e1.x() * e2.y() - e1.y() * e2.x();
        if (!(std::abs(det) > 0)) continue;
        for (int k = 0; k < sp.perElement(); ++k) {
            const int i = dof[k];
            if (done[std::size_t(i)]) continue;
            done[std::size_t(i)] = 1;
            const QPointF P = sp.point(i), r = P - g[0];
            ev.moveTo(t, (r.x() * e2.y() - r.y() * e2.x()) / det, (e1.x() * r.y() - e1.y() * r.x()) / det);
            double band[Carriers];
            bandsAt(ev, d, a_axis == 1 ? P.y() : P.x(), band);
            auto eV = [&](const Expression& e) {
                const double v = ev.eval(e);
                return e.dim() == Dim::of(2, 1, -2) ? v / Q : v;
            };
            for (int c = 0; c < Carriers; ++c) {
                if (!a_on[c] || a_states[c].empty()) continue;
                const bool electrons = c == 0;
                if (!(electrons ? a_etaN : a_etaP).isValid()) continue;
                // The Fermi level (eV): the band's edge here and the distance given.
                const double Ef = electrons ? band[0] + eV(a_etaN) : std::max(band[1], band[2]) - eV(a_etaP);
                if (!std::isfinite(Ef)) continue;
                double sum = 0;
                for (std::size_t j = 0; j < a_states[c].size(); ++j) {
                    const State& st = a_states[c][j];
                    const double psi = psiAt(st, i);
                    const double eta = (electrons ? Ef - st.E : st.E - Ef) * Q / kT;
                    const double add = psi * psi * scale * st.g * filled(st.mPar, eta);
                    sum += add;
                    total[c] += add;
                    if (st.top) highest[c] += add;   // (each ladder's highest: each valley set's)
                }
                a_density[electrons ? 0 : 1][std::size_t(i)] += sum;
            }
        }
    }
    // A well's carriers per area: each state's along its line, its |ψ|²
    // filled to the Fermi level at each point.
    if (a_1d) {
        for (int c = 0; c < Carriers; ++c)
            if (a_on[c] && (c == 0 ? a_etaN : a_etaP).isValid()) a_sheet[c].assign(a_states[c].size(), 0.0);
        for (const Point& pt : a_points) {
            if (pt.t < 0) continue;
            ev.moveTo(pt.t, pt.rx, pt.ry);
            auto eV = [&](const Expression& e) {
                const double v = ev.eval(e);
                return e.dim() == Dim::of(2, 1, -2) ? v / Q : v;
            };
            for (int c = 0; c < Carriers; ++c) {
                if (a_sheet[c].empty()) continue;
                const double Ef = c == 0 ? pt.pot[0] + eV(a_etaN) : std::max(-pt.pot[1], -pt.pot[2]) - eV(a_etaP);
                if (!std::isfinite(Ef)) continue;
                for (std::size_t j = 0; j < a_states[c].size(); ++j) {
                    const State& st = a_states[c][j];
                    const double psi = at(pt, st.x), eta = (c == 0 ? Ef - st.E : st.E - Ef) * Q / kT;
                    a_sheet[c][j] += pt.w * psi * psi * st.g * filled(st.mPar, eta);
                }
            }
        }
        if (a_sets > 0 && !a_sheet[0].empty()) {
            a_sheetValley.assign(std::size_t(a_sets), 0.0);
            for (std::size_t j = 0; j < a_states[0].size(); ++j)
                if (a_states[0][j].valley >= 0) a_sheetValley[std::size_t(a_states[0][j].valley)] += a_sheet[0][j];
        }
    }
    for (int c = 0; c < Carriers; ++c)
        if (total[c] > 0 && highest[c] > 1e-3 * total[c])
            a_s.log << tr("Warning: %1: its highest %2 state found holds %3 % of its carriers: ask for more states")
                           .arg(a_node->name(), QLatin1String(CarrierName[c]))
                           .arg(100 * highest[c] / total[c], 0, 'f', 1);
}

void QuantumProblem::selectMode(int k)
{
    Field& f = field();
    a_selected = k;
    const Space& sp = *f.space;
    const int nd = sp.size();
    const double scale = std::sqrt(densityScale());
    // (Each carrier's wavefunction, then the carriers in all the states
    // found: electrons, holes.)
    f.values.assign(std::size_t(nd) * std::size_t(Carriers + 2), 0.0);
    for (int c = 0; c < 2; ++c)
        if (a_density[c].size() == std::size_t(nd))
            std::copy(a_density[c].begin(), a_density[c].end(), f.values.begin() + std::ptrdiff_t(std::size_t(Carriers + c) * std::size_t(nd)));
    const std::size_t n1 = a_1d ? a_z.size() : 0;
    f.extra.assign(std::size_t(2 * Carriers) + Carriers * n1, NaN);
    // A 1D state's value at a node, and anywhere along the line (m).
    auto along = [&](const Eigen::VectorXd& x, std::size_t node) { return a_eq[node] >= 0 ? x(a_eq[node]) : 0.0; };
    for (int c = 0; c < Carriers; ++c) {
        if (k < 0 || k >= int(a_states[c].size())) continue;
        const State& s = a_states[c][std::size_t(k)];
        f.extra[std::size_t(c)] = s.E * Q;
        double peak = 0;
        if (a_1d) {
            for (std::size_t i = 0; i < n1; ++i) {
                const double v = along(s.x, i) * scale;
                f.extra[std::size_t(2 * Carriers) + std::size_t(c) * n1 + i] = v;
                peak = std::max(peak, v * v);
            }
            // The 2D degrees of freedom: the line's value where they project.
            const QPointF u = (a_E - a_S) / a_L;
            for (int dof = 0; dof < nd; ++dof) {
                const QPointF p = sp.point(dof) - a_S;
                const double z = p.x() * u.x() + p.y() * u.y();
                if (z < 0 || z > a_L) continue;
                const auto it = std::upper_bound(a_z.begin(), a_z.end(), z);
                std::size_t e = std::size_t(std::max<std::ptrdiff_t>(0, (it - a_z.begin()) - 1)) / 2;
                e = std::min(e, a_elemDomain.size() - 1);
                const double z0 = a_z[2 * e], z2 = a_z[2 * e + 2];
                const double xi = z2 > z0 ? (z - z0) / (z2 - z0) : 0;
                const double v = (1 - xi) * (1 - 2 * xi) * along(s.x, 2 * e) + 4 * xi * (1 - xi) * along(s.x, 2 * e + 1)
                                 + xi * (2 * xi - 1) * along(s.x, 2 * e + 2);
                f.values[std::size_t(c) * std::size_t(nd) + std::size_t(dof)] = v * scale;
            }
        } else {
            for (int dof = 0; dof < nd; ++dof) {
                const int eq = a_eq[std::size_t(dof)];
                if (eq < 0) continue;
                const double v = s.x(eq) * scale;
                f.values[std::size_t(c) * std::size_t(nd) + std::size_t(dof)] = v;
                peak = std::max(peak, v * v);
            }
        }
        f.extra[std::size_t(Carriers + c)] = peak;
    }
    a_s.setMode(k + 1);
}

bool QuantumProblem::solve(const SolveOptions& options, QString* error)
{
    // At rest: each carrier's four lowest states, the first selected.
    if (solveModes(options, 4, 0, error) < 0) return false;
    selectMode(0);
    return true;
}

double QuantumProblem::at(const Point& pt, const Eigen::VectorXd& x) const
{
    double v = 0;
    for (int i = 0; i < pt.n; ++i)
        if (pt.eq[i] >= 0) v += pt.N[i] * x(pt.eq[i]);
    return v;
}

std::complex<double> QuantumProblem::at(const Point& pt, const Eigen::VectorXcd& x, int offset) const
{
    std::complex<double> v = 0;
    for (int i = 0; i < pt.n; ++i)
        if (pt.eq[i] >= 0) v += pt.N[i] * x(offset + pt.eq[i]);
    return v;
}

QString QuantumProblem::kpSpuriousSaid() const
{
    if (a_kpSpuriousKs == 0) return {};
    return tr("; spurious states left out - not their band's, in the gap, or oscillating from node to node - at %1 of its %2 values of k%3%4")
        .arg(a_kpSpuriousKs)
        .arg(a_kpKs)
        .arg(a_kpSpurious[0] + a_kpSpurious[1] == 0 ? QString()
                                                     : tr(", at k = 0 %1 among the electrons and %2 among the holes (their own band's share %3)")
                                                           .arg(a_kpSpurious[0])
                                                           .arg(a_kpSpurious[1])
                                                           .arg(a_kpSpuriousShares.join(QStringLiteral(", "))))
        .arg(a_kpSpuriousKept[0] + a_kpSpuriousKept[1] == 0
                 ? QString()
                 : tr("; taken all the same, too few else: %1 electrons' at %2 values of k, %3 holes' at %4")
                       .arg(a_kpSpuriousKept[0])
                       .arg(a_kpKeptKs[0])
                       .arg(a_kpSpuriousKept[1])
                       .arg(a_kpKeptKs[1]));
}

QString QuantumProblem::kpDirection() const
{
    switch (a_inplane) {
    case 1: return tr("k along [100]");
    case 2: return tr("k along [110]");
    case 3: return tr("averaged over the plane's directions");
    default: return tr("the axial approximation");
    }
}

bool QuantumProblem::kpBands(int count, double kmax, int nk, QString* error, int conduction)
{
    using cd = std::complex<double>;
    a_kps.clear();
    const int n = a_neq, nb = a_valence;
    if (n <= 0 || (nb != 4 && nb != 6 && nb != 8)) return true;
    const cd I(0, 1);
    // X↑ Y↑ Z↑ X↓ Y↓ Z↓ (orbital o, spin s: 3 s + o) - the 8x8's S↑ S↓ after
    // them. Spin and orbit: Δ ((σ·L) - 1)/3, L's (Lk)ij = -i εkij - the j =
    // 3/2 states at 0, j = 1/2 at -Δ.
    Eigen::Matrix<cd, 6, 6> so = Eigen::Matrix<cd, 6, 6>::Zero(), sigmaL;
    {
        auto eps = [](int a, int b, int c) { return (a == b || b == c || a == c) ? 0 : ((b - a + 3) % 3 == 1 ? 1 : -1); };
        Eigen::Matrix2cd sigma[3];
        sigma[0] << 0, 1, 1, 0;
        sigma[1] << 0, -I, I, 0;
        sigma[2] << 1, 0, 0, -1;
        for (int s1 = 0; s1 < 2; ++s1)
            for (int s2 = 0; s2 < 2; ++s2)
                for (int o1 = 0; o1 < 3; ++o1)
                    for (int o2 = 0; o2 < 3; ++o2) {
                        cd v = 0;
                        for (int k = 0; k < 3; ++k) v += sigma[k](s1, s2) * (-I * double(eps(k, o1, o2)));
                        so(3 * s1 + o1, 3 * s2 + o2) = v;
                    }
        sigmaL = so;
        so = (so - Eigen::Matrix<cd, 6, 6>::Identity()) / 3.0;
    }
    // The basis: all six (eight), or the j = 3/2 four (so's eigenvalue 0) -
    // the 4x4 Luttinger-Kohn bands, the split-off band left out.
    Eigen::MatrixXcd U = Eigen::MatrixXcd::Identity(nb == 8 ? 8 : 6, nb == 8 ? 8 : 6);
    if (nb == 4) {
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix<cd, 6, 6>> es(so);
        U = es.eigenvectors().rightCols(4);   // (eigenvalues -1, -1, 0, 0, 0, 0)
    }
    // The 8x8: each domain's gap, its edges' difference at the points (the
    // field's potential moves both).
    std::vector<double> gapOf(a_bands.size(), 0.0), gapW(a_bands.size(), 0.0);
    double cEdge = Inf();   // the conduction band's lowest edge
    if (nb == 8)
        for (const Point& pt : a_points) {
            if (pt.d < 0 || std::size_t(pt.d) >= a_bands.size()) continue;
            gapOf[std::size_t(pt.d)] += pt.w * (pt.pot[0] + pt.pot[1]);
            gapW[std::size_t(pt.d)] += pt.w;
            cEdge = std::min(cEdge, pt.pot[0]);
        }
    a_kpRescaled.clear();
    a_kpSpurious[0] = a_kpSpurious[1] = a_kpSpuriousKs = 0;
    a_kpSpuriousKept[0] = a_kpSpuriousKept[1] = a_kpKeptKs[0] = a_kpKeptKs[1] = 0;
    a_kpKs = nk;
    a_kpSpuriousShares.clear();
    // The in-plane directions: one - the axial approximation's (Chuang's:
    // the plane isotropic, R's γ2 and γ3 their mean γ̄) or [100] or [110] -,
    // or the warped bands averaged over the plane's directions (D2d: 0 to
    // 45°, Gauss and Legendre's four).
    std::vector<std::pair<double, double>> directions;   // θ, its share
    bool wurtzite = false;   // (its c-plane isotropic: one direction for the average)
    for (const Band& b : a_bands) wurtzite = wurtzite || (b.on && b.wz);
    if (a_inplane == 3 && !wurtzite) {
        const double xg[4] = {-0.8611363115940526, -0.3399810435848563, 0.3399810435848563, 0.8611363115940526};
        const double wg[4] = {0.3478548451374538, 0.6521451548625461, 0.6521451548625461, 0.3478548451374538};
        for (int g = 0; g < 4; ++g) directions.emplace_back(M_PI / 8 * (1 + xg[g]), wg[g] / 2);
    } else
        directions.emplace_back(a_inplane == 2 ? M_PI / 4 : 0.0, 1.0);
    const bool axial = a_inplane == 0;
    for (const auto& [theta, share] : directions) {
    KpBands out;
    out.theta = theta;
    out.weight = share;
    // Each domain's terms (eV, nm): along the axis kz² (M, M, L), in the
    // plane along θ (kx, ky = k cos θ, k sin θ) kx² (L, M, M) + ky² (M, L,
    // M) + kx ky N (X-Y), k kz the X-Z and Y-Z couplings (N) - Dresselhaus,
    // Kip and Kittel's L = -(γ1 + 4γ2), M = -(γ1 - 2γ2), N = -6γ3 -, and at k
    // = 0 the Z orbital's strain and the spin and orbit's coupling. Axial:
    // along x with the X - Y anisotropy (L - M = -6γ2) -6γ̄'s. The X-Z
    // coupling in Burt and Foreman's order, kx N+ kz + kz N- kx (N- = M -
    // ħ²/2m0, N+ = N - N-): kz's neighbours where they are - symmetrized,
    // (N kz + kz N)/2, an interface of InGaAs and InP grows a spurious band
    // into the gap. Its terms by kz's side: xz the coefficient of -i ψ_i ψ_j'
    // (the X-Z element N+, the Z-X N-), its adjoint of i ψ_i' ψ_j.
    // The 8x8 (Kane's): S↑ S↓ coupled to the X, Y, Z of their spin by i P
    // kα (P = √(Ep ħ²/2m0)), the kz's in Burt and Foreman's order, i P kz in
    // S's row and -i kz P in Z's - the S band's part of N+ (kx N+ kz) - z1
    // the coefficient of -i ψ_i ψ_j' as xz's, x1 of k in the plane -, S's own
    // A_c ħ²k²/2m0 at the conduction band's edge; the valence bands' γ and
    // A_c what the remote bands leave (Foreman's: γ1 - Ep/3Eg, γ2,3 -
    // Ep/6Eg, A_c = 1/me - (Ep/3)(2/Eg + 1/(Eg + Δ))). A_c below 0 makes
    // spurious states in the gap, and at 0 S has no stiffness of its own -
    // the elements' first derivatives leave S's of none at the band's edge:
    // A_c below 1 (a free electron's), Ep lowered to make it 1 (Foreman's
    // other choice; GaAs's 28.8 eV to 22.5).
    // Wurtzite (c along the axis; Chuang and Chang's, Lumerical's): X L1 kx²
    // + M1 ky² + M2 kz², Y M1 kx² + L1 ky² + M2 kz², Z M3 (kx² + ky²) + L2 kz²,
    // X-Y N1 kx ky, X-Z and Y-Z N2 k kz - L1 = A2 + A4 + A5, M1 = A2 + A4 -
    // A5, M2 = A1 + A3, L2 = A1, M3 = A2, N1 = 2 A5, N2 = √2 A6 (ħ²/2m0) -; at
    // k = 0 Δ1 + λε + θε on X and Y, λε on Z, Δ2 Lz σz + Δ3 (Lx σx + Ly
    // σy) (Δ2 = Δ3 = Δso/3: Δso/3 σ·L), from the reference: the heavy holes'
    // edge less its Δ1 + Δ2 + λε + θε. N2 in Burt and Foreman's order: N2- the
    // E1 remote bands' alone, which give M2's and M3's remote parts too -
    // their geometric mean, zinc blende's M - ħ²/2m0 where the two are one.
    struct Terms {
        Eigen::MatrixXcd z2, x2, xz, d0, z1, x1;
        double ref = 0;   // the k·p's reference under the heavy holes' edge
    };
    std::vector<Terms> terms(a_bands.size());
    for (std::size_t d = 0; d < a_bands.size(); ++d) {
        const Band& b = a_bands[d];
        if (!b.on) continue;
        if (b.wz) {
            const double* A = b.A;
            const double L1 = (A[1] + A[3] + A[4]) * HB2M, M1 = (A[1] + A[3] - A[4]) * HB2M, M2 = (A[0] + A[2]) * HB2M, L2 = A[0] * HB2M,
                         M3 = A[1] * HB2M, N1 = 2 * A[4] * HB2M, N2 = std::sqrt(2.0) * A[5] * HB2M;
            const double remote = (M2 - HB2M) * (M3 - HB2M);
            const double N2m = remote > 0 ? std::copysign(std::sqrt(remote), M2 - HB2M) : N2 / 2, N2p = N2 - N2m;
            const double c = std::cos(theta), sn = std::sin(theta);
            Eigen::MatrixXcd z2 = Eigen::MatrixXcd::Zero(6, 6), x2 = z2, xz = z2, d0 = z2, z1 = z2, x1 = z2;
            for (int sp = 0; sp < 2; ++sp) {
                const int o = 3 * sp;
                z2(o, o) = M2;
                z2(o + 1, o + 1) = M2;
                z2(o + 2, o + 2) = L2;
                x2(o, o) = c * c * L1 + sn * sn * M1;
                x2(o + 1, o + 1) = sn * sn * L1 + c * c * M1;
                x2(o + 2, o + 2) = M3;
                x2(o, o + 1) = c * sn * N1;
                x2(o + 1, o) = c * sn * N1;
                xz(o, o + 2) = c * N2p;
                xz(o + 2, o) = c * N2m;
                xz(o + 1, o + 2) = sn * N2p;
                xz(o + 2, o + 1) = sn * N2m;
                d0(o, o) = b.dcr + b.lambdaE + b.thetaE;
                d0(o + 1, o + 1) = b.dcr + b.lambdaE + b.thetaE;
                d0(o + 2, o + 2) = b.lambdaE;
            }
            d0 += b.dso / 3 * sigmaL;
            terms[d] = {z2, x2, xz, d0, z1, x1, b.kpRef};
            continue;
        }
        if (!std::isfinite(b.g1) || !std::isfinite(b.g2) || !std::isfinite(b.g3)) {
            *error = tr("%1: the k·p valence bands want each material's Luttinger parameters (γ1, γ2, γ3)").arg(a_node->name());
            return false;
        }
        if (nb >= 6 && !std::isfinite(b.delta)) {
            *error = tr("%1: the %2x%2 k·p bands want each material's split-off energy Delta").arg(a_node->name()).arg(nb);
            return false;
        }
        double g1 = b.g1, g2 = b.g2, g3 = b.g3, Ac = 0, P = 0;
        if (nb == 8) {
            const double Eg = gapW[d] > 0 ? gapOf[d] / gapW[d] : NaN;
            if (!(Eg > 0) || !(b.Ep > 0) || !(b.me > 0)) {
                *error = tr("%1: the 8x8 k·p bands want each material's gap above 0, its Kane energy Ep and its electron mass (domain %2)")
                             .arg(a_node->name())
                             .arg(d + 1);
                return false;
            }
            // (Likewise the light holes' remote term along the axis, L's -(γ1
            // + 4γ2 - Ep/Eg) ħ²/2m0 - Kane's P takes Ep/Eg from it: below 0,
            // the Z orbital's band turns up into the gap at large kz, a band
            // of spurious states in it and among the electrons - Chao and
            // Chuang's GaAs (γ1 6.8, γ2 1.9) at Ep 21.19 eV gave -0.48, in
            // Miller's well of the check of 21 October; below 1, Ep lowered
            // to make it 1 too.)
            double Ep = b.Ep;
            const double sum = 2 / Eg + 1 / (Eg + b.delta);
            const double forS = 3 * (1 / b.me - 1) / sum, forZ = Eg * (b.g1 + 4 * b.g2 - 1);
            if (Ep > forS * (1 + 1e-9) || Ep > forZ * (1 + 1e-9)) {
                Ep = std::max(0.0, std::min(forS, forZ));
                if (theta == directions.front().first)
                    a_kpRescaled << tr("domain %1's Ep %2 eV to %3 eV%4")
                                        .arg(d + 1)
                                        .arg(formatNumber(b.Ep, 4), formatNumber(Ep, 4),
                                             forZ < forS ? tr(" (its light holes', γ1 + 4γ2 %1)").arg(formatNumber(b.g1 + 4 * b.g2, 4)) : QString());
            }
            Ac = 1 / b.me - Ep / 3 * sum;
            g1 -= Ep / (3 * Eg);
            g2 -= Ep / (6 * Eg);
            g3 -= Ep / (6 * Eg);
            P = std::sqrt(Ep * HB2M);
        }
        const double L = -(g1 + 4 * g2) * HB2M, M = -(g1 - 2 * g2) * HB2M, N = -6 * g3 * HB2M;
        const double Nm = M - HB2M, Np = N - Nm;
        const double c = std::cos(theta), sn = std::sin(theta), dA = axial ? -3 * (0.5 * (g2 + g3) - g2) * HB2M : 0.0;
        const int nf = nb == 8 ? 8 : 6;
        Eigen::MatrixXcd z2 = Eigen::MatrixXcd::Zero(nf, nf), x2 = z2, xz = z2, d0 = z2, z1 = z2, x1 = z2;
        for (int sp = 0; sp < 2; ++sp) {
            const int o = 3 * sp;
            z2(o, o) = M;
            z2(o + 1, o + 1) = M;
            z2(o + 2, o + 2) = L;
            x2(o, o) = c * c * L + sn * sn * M + dA;
            x2(o + 1, o + 1) = sn * sn * L + c * c * M - dA;
            x2(o + 2, o + 2) = M;
            x2(o, o + 1) = c * sn * N;
            x2(o + 1, o) = c * sn * N;
            xz(o, o + 2) = c * Np;
            xz(o + 2, o) = c * Nm;
            xz(o + 1, o + 2) = sn * Np;
            xz(o + 2, o + 1) = sn * Nm;
            d0(o + 2, o + 2) = b.dZ;
            if (nb == 8) {
                const int S = 6 + sp;
                z2(S, S) = Ac * HB2M;
                x2(S, S) = Ac * HB2M;
                x1(S, o) = I * P * c;
                x1(o, S) = -I * P * c;
                x1(S, o + 1) = I * P * sn;
                x1(o + 1, S) = -I * P * sn;
                z1(S, o + 2) = I * P;
            }
        }
        if (nb >= 6) d0.topLeftCorner(6, 6) += b.delta * so;
        terms[d] = {U.adjoint() * z2 * U, U.adjoint() * x2 * U, U.adjoint() * xz * U, U.adjoint() * d0 * U, U.adjoint() * z1 * U, U.adjoint() * x1 * U, 0.0};
    }
    // H(k) = H0 + k H1 + k² H2 (k in 1/nm) and M, by component then equation.
    const int N6 = nb * n;
    std::vector<Eigen::Triplet<cd>> t0, t1, t2;
    std::vector<Eigen::Triplet<double>> tm;
    double edge = -Inf();
    for (const Point& pt : a_points) {
        if (pt.d < 0 || std::size_t(pt.d) >= terms.size() || terms[std::size_t(pt.d)].z2.size() == 0) continue;
        const Terms& T = terms[std::size_t(pt.d)];
        // (The heavy holes' edge here, the field's and potential's in: an
        // electron's energy of the X and Y orbitals; the 8x8's S the
        // conduction band's.)
        const double ev = -pt.pot[1];
        edge = std::max(edge, std::max(ev, -pt.pot[2]));
        for (int i = 0; i < pt.n; ++i) {
            if (pt.eq[i] < 0) continue;
            for (int j = 0; j < pt.n; ++j) {
                if (pt.eq[j] < 0) continue;
                const double mass = pt.w * pt.N[i] * pt.N[j], stiff = pt.w * pt.ga[i] * pt.ga[j];
                const double right = pt.w * pt.N[i] * pt.ga[j], left = pt.w * pt.ga[i] * pt.N[j];   // ψ_i ψ_j', ψ_i' ψ_j
                for (int a = 0; a < nb; ++a) {
                    tm.emplace_back(a * n + pt.eq[i], a * n + pt.eq[j], mass);
                    for (int c = 0; c < nb; ++c) {
                        const cd h0 = stiff * T.z2(a, c) + mass * (T.d0(a, c) + (a == c ? (a < 6 ? ev - T.ref : pt.pot[0]) : 0.0))
                                      - I * (right * T.z1(a, c) - left * std::conj(T.z1(c, a)));
                        if (h0 != cd(0)) t0.emplace_back(a * n + pt.eq[i], c * n + pt.eq[j], h0);
                        const cd h1 = -I * (right * T.xz(a, c) - left * std::conj(T.xz(c, a))) + mass * T.x1(a, c);
                        if (h1 != cd(0)) t1.emplace_back(a * n + pt.eq[i], c * n + pt.eq[j], h1);
                        if (T.x2(a, c) != cd(0)) t2.emplace_back(a * n + pt.eq[i], c * n + pt.eq[j], mass * T.x2(a, c));
                    }
                }
            }
        }
    }
    Eigen::SparseMatrix<cd> H0(N6, N6), H1(N6, N6), H2(N6, N6);
    Eigen::SparseMatrix<double> Mr(N6, N6);
    H0.setFromTriplets(t0.begin(), t0.end());
    H1.setFromTriplets(t1.begin(), t1.end());
    H2.setFromTriplets(t2.begin(), t2.end());
    Mr.setFromTriplets(tm.begin(), tm.end());
    const Eigen::SparseMatrix<cd> Mc = Mr.cast<cd>();
    const int want = std::min(2 * std::max(1, count), N6);
    const int wantC = nb == 8 ? std::min(2 * std::max(0, conduction), N6) : 0;
    // (The 8x8: the valence states below the gap's middle, the conduction
    // states above it.)
    const double mid = nb == 8 ? 0.5 * (edge + cEdge) : Inf();
    // The 8x8's spurious states - what a material's parameters leave after
    // Ep's lowering above: a band turning into the gap at large kz (before
    // the light holes' rule, Miller's 96 Å well of the check of 21 October
    // had them at 0.203 and 0.655 eV, its electrons from 0.624: S shares
    // 0.28 and 0.32, kz 33/nm). Left out: a state is its band's if it is
    // mostly of it, on its side of its band's edges, and its own part
    // smooth along the axis (ħ²kz²/2m0 below 2 eV - a real one's under
    // 0.1).
    auto shareOf = [&](const Eigen::VectorXcd& x, bool electron) {   // (its band's share)
        double own = 0, all = 0;
        for (int c = 0; c < nb; ++c) {
            const double w = std::max(0.0, x.segment(c * n, n).dot(Mc.block(c * n, c * n, n, n) * x.segment(c * n, n)).real());
            all += w;
            if ((c >= 6) == electron) own += w;
        }
        return all > 0 ? own / all : 0.0;
    };
    auto spurious = [&shareOf, nb, n, cEdge, edge, this](const Eigen::VectorXcd& x, double E, bool electron) {
        if (nb != 8) return false;
        if (shareOf(x, electron) < 0.5 || (electron ? E < cEdge - 1e-6 : E > edge + 1e-6)) return true;
        double d1 = 0, d0 = 0;
        for (const Point& pt : a_points)
            for (int c = electron ? 6 : 0; c < (electron ? 8 : 6); ++c) {
                cd v = 0, g = 0;
                for (int i = 0; i < pt.n; ++i)
                    if (pt.eq[i] >= 0) {
                        v += pt.N[i] * x(c * n + pt.eq[i]);
                        g += pt.ga[i] * x(c * n + pt.eq[i]);
                    }
                d1 += pt.w * std::norm(g);
                d0 += pt.w * std::norm(v);
            }
        return HB2M * d1 / std::max(d0, 1e-300) > 2;   // (ħ²kz²/2m0, eV: ga in 1/nm)
    };
    // (Said once: k = 0's left out, of the first direction, and how many
    // values of k left any out; those taken all the same, too few else, and
    // at how many values of k.)
    int lastLeftK = -1, lastKeptK[2] = {-1, -1};
    auto keptAnyway = [&](int ik, bool electron) {
        if (!a_kps.empty()) return;
        const int side = electron ? 0 : 1;
        ++a_kpSpuriousKept[side];
        if (ik != lastKeptK[side]) {
            ++a_kpKeptKs[side];
            lastKeptK[side] = ik;
        }
    };
    auto leftOut = [&](int ik, const Eigen::VectorXcd& x, bool electron) {
        if (!a_kps.empty()) return;
        if (ik != lastLeftK) {
            ++a_kpSpuriousKs;
            lastLeftK = ik;
        }
        if (ik != 0) return;
        ++a_kpSpurious[electron ? 0 : 1];
        if (a_kpSpuriousShares.size() < 6) a_kpSpuriousShares << formatNumber(shareOf(x, electron), 2);
    };
    double lowestC = NaN;   // (the lowest conduction state at the k before, eV)
    for (int ik = 0; ik < nk; ++ik) {
        const double k = nk > 1 ? kmax * ik / (nk - 1) : 0;   // 1/m
        const double kn = k * 1e-9;                           // 1/nm
        Eigen::SparseMatrix<cd> H = H0 + kn * H1 + kn * kn * H2;
        std::vector<std::pair<double, Eigen::VectorXcd>> found, foundC;
        if (N6 <= 120 || 4 * (want + wantC) >= N6) {   // (dense: small, or most of it wanted)
            const Eigen::MatrixXcd Hd(H), Md(Mc);
            Eigen::GeneralizedSelfAdjointEigenSolver<Eigen::MatrixXcd> es(Hd, Md);
            if (es.info() != Eigen::Success) {
                *error = tr("%1: the k·p eigenproblem was not solved at k = %2").arg(a_node->name(), formatQuantity(k, Dim::of(-1)));
                return false;
            }
            // (The 8x8's spurious ones left out; too few else, the nearest
            // of them taken - each k its states -, said.)
            for (const bool electrons : {false, true}) {
                std::vector<std::pair<double, Eigen::VectorXcd>>& into = electrons ? foundC : found;
                std::vector<std::pair<double, Eigen::VectorXcd>> spare;
                const int wanted = electrons ? wantC : want;
                for (int c = 0; c < N6 && int(into.size()) < wanted; ++c) {
                    const int m = electrons ? c : N6 - 1 - c;
                    if (electrons ? !(es.eigenvalues()(m) > mid) : !(es.eigenvalues()(m) < mid)) continue;
                    Eigen::VectorXcd x = es.eigenvectors().col(m);
                    if (spurious(x, es.eigenvalues()(m), electrons)) {
                        leftOut(ik, x, electrons);
                        spare.emplace_back(es.eigenvalues()(m), std::move(x));
                    } else
                        into.emplace_back(es.eigenvalues()(m), std::move(x));
                }
                for (std::size_t c = 0; c < spare.size() && int(into.size()) < wanted; ++c) {
                    into.push_back(spare[c]);
                    keptAnyway(ik, electrons);
                }
                std::stable_sort(into.begin(), into.end(), [electrons](const auto& a, const auto& b) { return electrons ? a.first < b.first : a.first > b.first; });
            }
        } else {
            // Shift and invert at σ: above the highest valence edge, (σM - H)
            // - positive definite but for the 8x8's conduction states -, its
            // inverse's largest eigenvalues the highest states; below the
            // 8x8's lowest conduction edge, (H - σM), the lowest.
            auto states = [&H, &Mc, N6, nb, error, this, mid, ik, &spurious, &leftOut, &keptAnyway](double sigma, bool above, int wantedHere, std::vector<std::pair<double, Eigen::VectorXcd>>& into) {
                // (The 8x8's spurious ones among them: more sought while too
                // few pass - none more at first, which made every 8x8 search
                // 2.4 times as slow.)
                int howManyWanted = std::min(wantedHere, N6);
                Eigen::SparseMatrix<cd> A = above ? Eigen::SparseMatrix<cd>(H - sigma * Mc) : Eigen::SparseMatrix<cd>(sigma * Mc - H);
                A.makeCompressed();
                std::function<Eigen::VectorXcd(const Eigen::VectorXcd&)> solve;
                Eigen::SimplicialLDLT<Eigen::SparseMatrix<cd>> ldlt;
                SparseLu<cd> lu;
                if (nb == 8) {
                    lu.analyzePattern(A);
                    lu.factorize(A);
                    if (lu.info() != Eigen::Success) {
                        *error = tr("%1: the shifted 8x8 k·p matrix is singular (a state at the shift?)").arg(a_node->name());
                        return false;
                    }
                    solve = [&](const Eigen::VectorXcd& v) { return Eigen::VectorXcd(lu.solve(v)); };
                } else {
                    ldlt.compute(A);
                    if (ldlt.info() != Eigen::Success) {
                        *error = tr("%1: the shifted k·p matrix is not positive definite (a spurious state?)").arg(a_node->name());
                        return false;
                    }
                    solve = [&](const Eigen::VectorXcd& v) { return Eigen::VectorXcd(ldlt.solve(v)); };
                }
                // Krylov and Schur's largest of the inverse times M, then again
                // on what is M-orthogonal to all found so far, till a search
                // finds nothing as high as the lowest state wanted; Rayleigh
                // and Ritz over all found. One vector's Krylov space holds one
                // direction of each degenerate set - a Kramers pair's other
                // only as rounding brings it -; the other lies in what is
                // orthogonal to the found. (The 8x8: the other side's states
                // - θ below 0 - kept in the basis too, out of the searches'
                // way, and left out after.)
                std::vector<Eigen::VectorXcd> basis, mbasis;   // M-orthonormal; M times each
                auto add = [&](Eigen::VectorXcd v) {
                    const double before = std::sqrt(std::max(0.0, v.dot(Mc * v).real()));
                    for (int pass = 0; pass < 2; ++pass)
                        for (std::size_t c = 0; c < basis.size(); ++c) v -= basis[c] * mbasis[c].dot(v);
                    const Eigen::VectorXcd mv = Mc * v;
                    const double norm = std::sqrt(std::max(0.0, v.dot(mv).real()));
                    if (norm > 1e-8 * std::max(before, 1e-300)) {
                        basis.push_back(v / norm);
                        mbasis.push_back(mv / norm);
                    }
                };
                auto project = [&](Eigen::VectorXcd& v) {
                    for (std::size_t c = 0; c < basis.size(); ++c) v -= basis[c] * mbasis[c].dot(v);
                };
                // (After the first, a search of two, to 1e-5, checks the rest;
                // finding one as high (to 1e-4), a search of the pairs' number
                // follows. A state just below the wanted ones found so too is
                // harmless: Rayleigh and Ritz keep the highest.)
                std::vector<std::pair<double, Eigen::VectorXcd>> spare;   // (the last round's left out, by energy)
                for (int round = 0; round < 3; ++round) {
                    double lowest = Inf();   // the lowest wanted state's θ = 1/(σ - E) so far
                    bool full = true;
                    for (int search = 0; search < 8; ++search) {
                        const int howMany = std::min(search == 0 ? howManyWanted + 4 : full ? howManyWanted / 2 + 4 : 2, N6 - int(basis.size()) - 2);
                        if (howMany <= 0) break;
                        const bool check = search > 0 && !full;
                        const EigenResult r = largestEigenvalues(
                            N6, howMany,
                            [&](const Eigen::VectorXcd& x, Eigen::VectorXcd& y) {
                                Eigen::VectorXcd v = x;
                                project(v);
                                y = solve(Mc * v);
                                project(y);
                            },
                            0, check ? 1e-5 : 1e-10, 300, [this] { return cancelled(); });
                        if (r.cancelled) {
                            *error = tr("%1: cancelled").arg(a_node->name());
                            return false;
                        }
                        if (!r.error.isEmpty()) a_s.log << tr("Warning: %1: %2").arg(a_node->name(), r.error);
                        bool higher = false;
                        for (std::size_t c = 0; c < r.vectors.size(); ++c) {
                            const bool other = r.values[c].real() < 0;
                            if (search > 0 && !other && !(r.values[c].real() > lowest * (1 - 1e-4))) continue;
                            higher = higher || (search > 0 && !other);
                            if (!check || other) add(r.vectors[c]);
                        }
                        // The θ of the want-th highest found: the bar for another search.
                        std::vector<double> thetas;
                        for (const std::complex<double>& t : r.values)
                            if (t.real() > 0) thetas.push_back(t.real());
                        if (search == 0) {
                            std::sort(thetas.rbegin(), thetas.rend());
                            lowest = thetas.empty() ? Inf() : thetas[std::size_t(std::min<int>(howManyWanted, int(thetas.size())) - 1)];
                            full = false;
                        } else if (!higher && !full)
                            break;
                        else
                            full = !full && higher;
                    }
                    const int m = int(basis.size());
                    Eigen::MatrixXcd B(N6, m);
                    for (int c = 0; c < m; ++c) B.col(c) = basis[std::size_t(c)];
                    const Eigen::MatrixXcd small = B.adjoint() * (H * B);
                    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXcd> es(0.5 * (small + small.adjoint()));
                    // (The 8x8's spurious ones left out - more were searched for
                    // than wanted, the wanted ones the next.)
                    into.clear();
                    spare.clear();
                    auto take = [&](int c) {
                        Eigen::VectorXcd x = B * es.eigenvectors().col(c);
                        if (spurious(x, es.eigenvalues()(c), above)) {
                            spare.emplace_back(es.eigenvalues()(c), std::move(x));
                            return;
                        }
                        into.emplace_back(es.eigenvalues()(c), std::move(x));
                    };
                    if (above) {
                        for (int c = 0; c < m && int(into.size()) < wantedHere; ++c)
                            if (es.eigenvalues()(c) > mid) take(c);
                    } else
                        for (int c = m - 1; c >= 0 && int(into.size()) < wantedHere; --c)
                            if (es.eigenvalues()(c) < mid) take(c);
                    if (int(into.size()) >= wantedHere || howManyWanted >= N6 / 2) break;
                    howManyWanted = std::min(2 * howManyWanted + 8, N6 / 2);
                }
                for (const auto& [E, x] : spare) leftOut(ik, x, above);
                // (Too few still: the nearest left out taken - each k its
                // states, the bands' arrays whole -, said.)
                for (std::size_t c = 0; c < spare.size() && int(into.size()) < wantedHere; ++c) {
                    into.push_back(spare[c]);
                    keptAnyway(ik, above);
                }
                std::stable_sort(into.begin(), into.end(), [above](const auto& a, const auto& b) { return above ? a.first < b.first : a.first > b.first; });
                return true;
            };
            if (!states(edge + 0.02 + 1e-3 * std::abs(edge), false, want, found)) return false;
            // (The conduction states' shift below the lowest found at the k
            // before - they rise with k -, not at the band's edge: a
            // spurious band stays near the gap as they rise, and would fill
            // the search there - none of the electrons found at 2.7/nm in the
            // check of 21 October.)
            double sigmaC = cEdge - 0.02 - 1e-3 * std::abs(cEdge);
            if (std::isfinite(lowestC)) sigmaC = std::max(sigmaC, lowestC - 0.05 - 1e-3 * std::abs(lowestC));
            if (wantC > 0 && !states(sigmaC, true, wantC, foundC)) return false;
        }
        if (!foundC.empty()) {
            lowestC = Inf();
            for (const auto& f : foundC) lowestC = std::min(lowestC, f.first);
        }

        std::vector<double> energies, energiesC;
        std::vector<Eigen::VectorXcd> vectors, vectorsC, sParts;
        for (auto& [E, x] : found) {
            x /= std::sqrt(std::max(1e-300, (x.adjoint() * (Mc * x))(0).real()));
            // In X↑ Y↑ Z↑ X↓ Y↓ Z↓ (the 8x8's S parts apart).
            Eigen::VectorXcd x6 = Eigen::VectorXcd::Zero(6 * n);
            for (int c = 0; c < 6; ++c)
                for (int a = 0; a < nb; ++a)
                    if (U(c, a) != cd(0)) x6.segment(c * n, n) += U(c, a) * x.segment(a * n, n);
            energies.push_back(E);
            vectors.push_back(std::move(x6));
            if (nb == 8) sParts.push_back(x.segment(6 * n, 2 * n));
        }
        for (auto& [E, x] : foundC) {
            x /= std::sqrt(std::max(1e-300, (x.adjoint() * (Mc * x))(0).real()));
            energiesC.push_back(E);
            vectorsC.push_back(std::move(x));
        }
        out.k.push_back(k);
        out.E.push_back(std::move(energies));
        out.x.push_back(std::move(vectors));
        if (nb == 8) {
            out.C.push_back(std::move(energiesC));
            out.xc.push_back(std::move(vectorsC));
            out.xs.push_back(std::move(sParts));
        }
    }
    // The states along k (KpBands::order): each k's matched to the k
    // before's by their overlaps |<a|b>|² (the mass matrix lumped), largest
    // first, among the 8 either side by energy; one leaving the states found
    // given what is left.
    {
        Eigen::VectorXd lumped = Eigen::VectorXd::Zero(n);
        for (int c = 0; c < Mr.outerSize(); ++c)
            for (Eigen::SparseMatrix<double>::InnerIterator it(Mr, c); it; ++it)
                if (it.row() < n && it.col() < n) lumped(it.row()) += it.value();
        auto track = [n, &lumped](const std::vector<std::vector<Eigen::VectorXcd>>& xs) {
            std::vector<std::vector<int>> order(xs.size());
            if (xs.empty()) return order;
            order[0].resize(xs[0].size());
            std::iota(order[0].begin(), order[0].end(), 0);
            for (std::size_t m = 1; m < xs.size(); ++m) {
                const std::vector<int>& before = order[m - 1];
                const std::size_t nb4 = before.size(), now = xs[m].size();
                std::vector<std::tuple<double, int, int>> o;
                for (std::size_t a = 0; a < nb4; ++a) {
                    const Eigen::VectorXcd& x = xs[m - 1][std::size_t(before[a])];
                    const int at0 = before[a];
                    for (int b = std::max(0, at0 - 8); b < std::min(int(now), at0 + 9); ++b) {
                        const Eigen::VectorXcd& y = xs[m][std::size_t(b)];
                        std::complex<double> v = 0;
                        for (Eigen::Index c = 0; c * n < x.size() && c * n < y.size(); ++c)
                            v += (x.segment(c * n, n).conjugate().cwiseProduct(lumped).cwiseProduct(y.segment(c * n, n))).sum();
                        o.emplace_back(std::norm(v), int(a), b);
                    }
                }
                std::sort(o.begin(), o.end(), [](const auto& p, const auto& q) { return std::get<0>(p) > std::get<0>(q); });
                std::vector<int> to(nb4, -1);
                std::vector<char> taken(now, 0);
                for (const auto& [w, a, b] : o)
                    if (to[std::size_t(a)] < 0 && !taken[std::size_t(b)]) {
                        to[std::size_t(a)] = b;
                        taken[std::size_t(b)] = 1;
                    }
                std::size_t free = 0;
                for (int& t : to) {
                    if (t >= 0) continue;
                    while (free < now && taken[free]) ++free;
                    t = free < now ? int(free) : 0;
                    if (free < now) taken[free] = 1;
                }
                order[m] = std::move(to);
            }
            return order;
        };
        out.order = track(out.x);
        if (nb == 8) out.orderC = track(out.xc);
    }
    a_kps.push_back(std::move(out));
    }
    return true;
}

QuantumProblem::Exciton QuantumProblem::exciton(const State& e, const State& h, int c, double mr, double epsr, bool two) const
{
    auto inverse = [&](const Point& pt, int carrier) {
        const Band& b = a_bands[std::size_t(std::max(0, pt.d))];
        return 1 / (carrier == 0 ? b.me : carrier == 1 ? b.mhhZ : b.mlhZ);
    };
    return exciton([&](const Point& pt) { return std::pow(at(pt, e.x), 2); }, [&](const Point& pt) { return std::pow(at(pt, h.x), 2); },
                   [&](const Point& pt) { return inverse(pt, 0); }, [&](const Point& pt) { return inverse(pt, c); }, mr, epsr, two);
}

QuantumProblem::Exciton QuantumProblem::exciton(const std::function<double(const Point&)>& e, const std::function<double(const Point&)>& h,
                                                const std::function<double(const Point&)>& ie, const std::function<double(const Point&)>& ih, double mr,
                                                double epsr, bool two) const
{
    // Bastard's variational exciton: χe(ze) χh(zh) √(2/π)/λ e^(-ρ/λ), its
    // energy ħ²/(2 µ λ²) less q²/(4π ε) ∫ P(s) g(s/λ)/λ ds - P the
    // distribution of the electron's and hole's separation along the axis,
    // from their |χ|² binned - least over λ. With two parameters (Gerlach et
    // al.'s, Khramtsov et al.'s; Lumerical's variational one): χe χh
    // e^(-√(ρ² + γ² (ze - zh)²)/λ), the electron and hole drawn together
    // along the axis too - over the separations (x = s/λ, c = γx), its
    // energy [Σ P ((ħ²/2µλ²)(n - 4c² E1(2c)) - k v(x, γ)/λ) + (ħ²/2m0λ²) Σ
    // (Pe + Ph) 4γ²c² E1(2c)] / Σ P n, n = e^(-2c) (1 + 2c), Pe and Ph P
    // weighed by the electron's and hole's 1/m along the axis; least over λ
    // and γ (0 to 1.5); |F(0)|² = 2/(πλ² Σ P n). At γ 0 Bastard's.
    Exciton out;
    struct Sample {
        double z, e, h, ie, ih;
    };
    std::vector<Sample> at;
    at.reserve(a_points.size());
    double lo = Inf(), hi = -Inf();
    for (const Point& pt : a_points) {
        const double we = pt.w * e(pt), wh = pt.w * h(pt);
        at.push_back({pt.a, we, wh, two && we != 0 ? we * ie(pt) : 0.0, two && wh != 0 ? wh * ih(pt) : 0.0});
        lo = std::min(lo, pt.a);
        hi = std::max(hi, pt.a);
    }
    if (at.empty() || !(hi > lo) || !(mr > 0) || !(epsr > 0)) return out;
    const double bin = two ? std::max(1e-11, (hi - lo) / 400) : std::max(2e-11, (hi - lo) / 3000);
    const int n = int((hi - lo) / bin) + 1;
    std::vector<double> pe(std::size_t(n), 0.0), ph(std::size_t(n), 0.0), pie(std::size_t(n), 0.0), pih(std::size_t(n), 0.0);
    for (const Sample& sm : at) {
        const std::size_t k = std::size_t(std::clamp(int((sm.z - lo) / bin), 0, n - 1));
        pe[k] += sm.e;
        ph[k] += sm.h;
        pie[k] += sm.ie;
        pih[k] += sm.ih;
    }
    std::vector<int> je, jh;
    for (int k = 0; k < n; ++k) {
        if (pe[std::size_t(k)] > 1e-12) je.push_back(k);
        if (ph[std::size_t(k)] > 1e-12) jh.push_back(k);
    }
    std::vector<double> P(std::size_t(n), 0.0), Pz(std::size_t(n), 0.0);
    for (int i : je)
        for (int j : jh) {
            const std::size_t m = std::size_t(std::abs(i - j));
            P[m] += pe[std::size_t(i)] * ph[std::size_t(j)];
            Pz[m] += pie[std::size_t(i)] * ph[std::size_t(j)] + pe[std::size_t(i)] * pih[std::size_t(j)];
        }
    std::vector<std::array<double, 3>> sep;   // s (m), its share, its share weighed by 1/m
    for (int m = 0; m < n; ++m)
        if (P[std::size_t(m)] > 0) sep.push_back({m == 0 ? bin / 3 : m * bin, P[std::size_t(m)], Pz[std::size_t(m)]});
    const double k = Q * Q / (4 * M_PI * Eps0 * epsr);
    auto energy = [&](double lambda, double g, double* norm) {
        const double kin = Hbar * Hbar / (2 * mr * lambda * lambda), kinz = Hbar * Hbar / (2 * M0 * lambda * lambda);
        double num = 0, den = 0;
        for (const auto& [sm, p, pz] : sep) {
            const double x = sm / lambda, c = g * x;
            if (!(c > 0)) {
                num += p * (kin - k * excitonG(x) / lambda);
                den += p;
                continue;
            }
            const double nn = std::exp(-2 * c) * (1 + 2 * c), cE = c * c * expIntegralE1(2 * c);
            num += p * (kin * (nn - 4 * cE) - k * excitonV(x, g) / lambda) + pz * kinz * 4 * g * g * cE;
            den += p * nn;
        }
        if (norm) *norm = den;
        return den > 0 ? num / den : 0.0;
    };
    // Golden sections: in ln λ (0.2 nm to 2 µm; with γ, about Bastard's λ),
    // and in γ.
    auto golden = [](double a, double b, int iterations, const std::function<double(double)>& f) {
        const double r = (std::sqrt(5.0) - 1) / 2;
        double c = b - r * (b - a), d = a + r * (b - a), fc = f(c), fd = f(d);
        for (int it = 0; it < iterations; ++it) {
            if (fc < fd) {
                b = d;
                d = c;
                fd = fc;
                c = b - r * (b - a);
                fc = f(c);
            } else {
                a = c;
                c = d;
                fc = fd;
                d = a + r * (b - a);
                fd = f(d);
            }
        }
        return (a + b) / 2;
    };
    const double l0 = golden(std::log(2e-10), std::log(2e-6), 120, [&](double l) { return energy(std::exp(l), 0, nullptr); });
    double lambda = std::exp(l0), g = 0;
    if (two) {
        // (Flat at its least: γ to 1e-3, ln λ to 1e-5 are enough.)
        auto best = [&](double gg) { return golden(l0 - 1.5, l0 + 1.5, 26, [&](double l) { return energy(std::exp(l), gg, nullptr); }); };
        g = golden(0, 1.5, 15, [&](double gg) { return energy(std::exp(best(gg)), gg, nullptr); });
        // (No better than γ 0: Bastard's.)
        if (!(energy(std::exp(best(g)), g, nullptr) < energy(lambda, 0, nullptr))) g = 0;
        else lambda = std::exp(best(g));
    }
    double norm = 1;
    const double least = energy(lambda, g, &norm);
    if (least < 0 && norm > 0) {
        out.Eb = -least;
        out.radius = lambda;
        out.gamma = g;
        out.F0 = 2 / (M_PI * lambda * lambda * norm);
    }
    return out;
}

bool QuantumProblem::screen(const SpectrumOptions& o, QString* error)
{
    ++a_inner;
    const auto inner = qScopeGuard([this] { --a_inner; });
    // (The holes' k·p bands at 31 values of k to their reach; the levels as
    // the spectrum finds them: each state's carriers its band's.)
    const double kT = KB * a_T, scale = densityScale();
    const bool kp = a_valence > 0;
    const int count = (a_valence == 4 ? 2 : 3) * std::max(1, o.states);
    auto softplus = [](double x) { return x > 30 ? x : std::log1p(std::exp(x)); };
    double alpha = 0.3, last = Inf(), moved = Inf();
    int turn = 0;
    a_screening = true;
    const auto done = qScopeGuard([&] { a_screening = false; });
    std::vector<double> rho;
    for (turn = 1; turn <= 60; ++turn) {
        if (cancelled()) {
            *error = tr("%1: cancelled").arg(a_node->name());
            return false;
        }
        a_rho = rho;
        if (solveModes(o.solve, std::max(1, o.states), 0, error) < 0) return false;
        if (rho.empty()) rho.assign(a_points.size(), 0.0);
        const double want = o.density * a_activeSize;
        if (kp) {
            const double kh = std::sqrt(2 * 0.5 * M0 * (0.15 * Q + 15 * kT)) / Hbar;
            if (!kpBands(count, kh, 31, error, a_valence == 8 ? int(a_states[0].size()) : 0)) return false;
        }
        // Each band's carriers per area at a level F: a k·p band's over its
        // intervals (each linear in k²), a parabola's at once.
        auto kpSheet = [&](bool electrons, double F, const std::function<void(const KpBands&, std::size_t, std::size_t, double)>& each) {
            double total = 0;
            for (const KpBands& b : a_kps) {
                const std::vector<std::vector<double>>& E = electrons ? b.C : b.E;
                for (std::size_t j = 0; j < (E.empty() ? 0 : E[0].size()); ++j)
                    for (std::size_t m = 1; m < b.k.size(); ++m) {
                        const double Ea = E[m - 1][j] * Q, Eb = E[m][j] * Q, dk2 = b.k[m] * b.k[m] - b.k[m - 1] * b.k[m - 1];
                        const double x = electrons ? (F - 0.5 * (Ea + Eb)) / kT : (0.5 * (Ea + Eb) - F) / kT;
                        const double occ = std::abs(Eb - Ea) > 1e-6 * kT
                                               ? (electrons ? kT * (softplus((F - Ea) / kT) - softplus((F - Eb) / kT)) / (Eb - Ea)
                                                            : kT * (softplus((Eb - F) / kT) - softplus((Ea - F) / kT)) / (Eb - Ea))
                                               : 1 / (1 + std::exp(-x));
                        const double add = b.weight * dk2 / (4 * M_PI) * occ;
                        total += add;
                        if (each) each(b, j, m, add);
                    }
            }
            return total;
        };
        auto parabolic = [&](int c, double F) {
            double total = 0;
            for (const State& st : a_states[c]) {
                const double x = c == 0 ? (F - st.E * Q) / kT : (st.E * Q - F) / kT;
                total += st.g * st.mPar * M0 * kT / (M_PI * Hbar * Hbar) * softplus(x);
            }
            return total;
        };
        auto electronsAt = [&](double F) { return a_valence == 8 ? kpSheet(true, F, {}) : parabolic(0, F); };
        auto holesAt = [&](double F) { return kp ? kpSheet(false, F, {}) : parabolic(1, F) + parabolic(2, F); };
        auto level = [&](bool electrons) {
            double lo = -30 * Q, hi = 30 * Q;
            for (int it = 0; it < 200; ++it) {
                const double mid = (lo + hi) / 2;
                if (((electrons ? electronsAt(mid) : holesAt(mid)) < want) == electrons) lo = mid;
                else hi = mid;
            }
            return (lo + hi) / 2;
        };
        const double Fc = level(true), Fv = level(false);
        // Their densities at the points (1/m³).
        std::vector<double> n(a_points.size(), 0.0), p(a_points.size(), 0.0);
        auto spinor = [&](std::vector<double>& into, const Eigen::VectorXcd& a, const Eigen::VectorXcd& b, int parts, double w) {
            for (std::size_t i = 0; i < a_points.size(); ++i) {
                double v = 0;
                for (int c = 0; c < parts; ++c) v += std::norm(at(a_points[i], a, c * a_neq)) + std::norm(at(a_points[i], b, c * a_neq));
                into[i] += w / 2 * v * scale;
            }
        };
        if (a_valence == 8)
            kpSheet(true, Fc, [&](const KpBands& b, std::size_t j, std::size_t m, double add) { spinor(n, b.xc[m - 1][j], b.xc[m][j], 8, add); });
        else
            for (const State& st : a_states[0]) {
                const double sheet = st.g * st.mPar * M0 * kT / (M_PI * Hbar * Hbar) * softplus((Fc - st.E * Q) / kT);
                for (std::size_t i = 0; i < a_points.size(); ++i) n[i] += sheet * std::pow(at(a_points[i], st.x), 2) * scale;
            }
        if (kp)
            kpSheet(false, Fv, [&](const KpBands& b, std::size_t j, std::size_t m, double add) { spinor(p, b.x[m - 1][j], b.x[m][j], 6, add); });
        else
            for (int c = 1; c < Carriers; ++c)
                for (const State& st : a_states[c]) {
                    const double sheet = st.mPar * M0 * kT / (M_PI * Hbar * Hbar) * softplus((st.E * Q - Fv) / kT);
                    for (std::size_t i = 0; i < a_points.size(); ++i) p[i] += sheet * std::pow(at(a_points[i], st.x), 2) * scale;
                }
        // The charge mixed (a third, less when it overshoots), and how far
        // the potential would move.
        std::vector<double> next(rho.size());
        for (std::size_t i = 0; i < rho.size(); ++i) next[i] = rho[i] + alpha * (Q * (p[i] - n[i]) - rho[i]);
        const std::vector<double> U = polarizationEnergy(&next);
        moved = 0;
        for (std::size_t i = 0; i < U.size(); ++i) moved = std::max(moved, std::abs(U[i] - a_Upol[i]));
        rho = std::move(next);
        if (moved < 1e-4) break;
        if (moved > last) alpha = std::max(0.02, alpha / 2);
        last = moved;
    }
    a_rho = rho;
    a_screening = false;
    if (!(moved < 1e-4))
        a_s.log << tr("Warning: %1: the carriers' screening of the polarization still moves the bands by %2 after %3 turns")
                       .arg(a_node->name(), electronVolts(moved * Q))
                       .arg(turn - 1);
    else
        a_s.log << tr("%1: the polarization screened by the spectrum's carriers in %2 turns").arg(a_node->name()).arg(turn);
    return true;
}

int QuantumProblem::solveSpectrum(const SpectrumOptions& o, QString* error)
{
    ++a_inner;
    const auto inner = qScopeGuard([this] { --a_inner; });
    if (a_sets > 0) {
        *error = tr("%1: its electrons in valleys (Si, Ge: an indirect gap) have no optical spectrum here").arg(a_node->name());
        return -1;
    }
    a_rho.clear();
    if (a_polarization && o.density > 0 && !screen(o, error)) return -1;
    // (Choosing among them - by where they are, or their slope at the walls:
    // some more found than kept.)
    const bool choosing = o.select == 1 || std::isfinite(o.cutoffC) || std::isfinite(o.cutoffV);
    if (choosing && !a_1d) {
        *error = tr("%1: choosing states by their place or the walls, and an absorption per the stack or a length, are a well's (1D)")
                     .arg(a_node->name());
        return -1;
    }
    // The states that enter. Each one's slope at the line's ends (its hard
    // walls) as a share of its largest, and its share of |ψ|² in the wells
    // and the barriers between them, from its amplitude at the points.
    // (The points along the line in order, and the wells' stretch: once
    // solved.)
    std::vector<const Point*> along;
    double wellsLo = Inf(), wellsHi = -Inf(), ends[2] = {0, 0};
    auto alongTheLine = [this, &along, &wellsLo, &wellsHi, &ends]() {
        along.clear();
        for (const Point& pt : a_points) along.push_back(&pt);
        std::sort(along.begin(), along.end(), [](const Point* a, const Point* b) { return a->a < b->a; });
        wellsLo = Inf();
        wellsHi = -Inf();
        for (const Point* pt : along)
            if (pt->d >= 0 && std::size_t(pt->d) < a_active.size() && a_active[std::size_t(pt->d)]) {
                wellsLo = std::min(wellsLo, pt->a);
                wellsHi = std::max(wellsHi, pt->a);
            }
        ends[1] = a_L;   // (m, as the points' a)
    };
    const bool periodic = a_periodic;
    auto measure = [&along, &ends, &wellsLo, &wellsHi, periodic](const std::function<double(const Point&)>& amplitude, double* wall, double* inWells) {
        double most = 0, all = 0, in = 0, prevA = ends[0], prevV = 0, atWalls = 0;
        for (std::size_t k = 0; k < along.size(); ++k) {
            const Point& pt = *along[k];
            const double v = amplitude(pt), da = pt.a - prevA;
            if (da > 1e-12) {
                const double slope = std::abs(v - prevV) / da;
                most = std::max(most, slope);
                if (k == 0) atWalls = std::max(atWalls, slope);
            }
            prevA = pt.a;
            prevV = v;
            all += pt.w * v * v;
            if (pt.a >= wellsLo - 1e-9 && pt.a <= wellsHi + 1e-9) in += pt.w * v * v;
        }
        if (ends[1] - prevA > 1e-12) {
            const double slope = std::abs(prevV) / (ends[1] - prevA);
            most = std::max(most, slope);
            atWalls = std::max(atWalls, slope);
        }
        *wall = most > 0 && !periodic ? atWalls / most : 0.0;   // (periodic: no walls)
        *inWells = all > 0 ? in / all : 0.0;
    };
    auto passes = [&](double wall, double inWells, double cutoff) {
        return (!std::isfinite(cutoff) || wall <= cutoff) && (o.select == 0 || inWells >= o.localized);
    };
    // Electrons: the lowest that pass, cstates of them (else states).
    const int wantE = o.cstates > 0 ? o.cstates : o.states;
    std::vector<char> keepE;
    std::vector<char> keepH[Carriers];
    QStringList rejected;
    // (Found again with more of each while too few pass: a field's
    // triangle against a wall holds many.)
    for (int each = std::max({1, o.states, o.cstates, o.vstates}) + (choosing ? 4 : 0);; each = std::min(64, 2 * each)) {
        if (solveModes(o.solve, each, 0, error) < 0) return -1;
        if (!a_on[0] || (!a_on[1] && !a_on[2])) {
            *error = tr("%1: a spectrum wants electrons and holes").arg(a_node->name());
            return -1;
        }
        alongTheLine();
        keepE.assign(a_states[0].size(), 0);
        rejected.clear();
        int keptE = 0, keptH = 0, wantH = 0;
        {
            int kept = 0;
            for (std::size_t i = 0; i < a_states[0].size() && kept < wantE; ++i) {
                double wall = 0, inWells = 1;
                if (choosing) measure([&](const Point& pt) { return at(pt, a_states[0][i].x); }, &wall, &inWells);
                if (!passes(wall, inWells, o.cutoffC)) {
                    rejected << QStringLiteral("e%1").arg(i + 1);
                    continue;
                }
                keepE[i] = 1;
                ++kept;
            }
            keptE = kept;
        }
        // Parabolic holes: the highest that pass - vstates of heavy and light
        // together by energy, else states of each.
        for (int c = 1; c < Carriers; ++c) keepH[c].assign(a_states[c].size(), 0);
        {
            std::vector<std::pair<int, std::size_t>> holes;
            for (int c = 1; c < Carriers; ++c)
                for (std::size_t j = 0; j < a_states[c].size(); ++j) holes.emplace_back(c, j);
            std::stable_sort(holes.begin(), holes.end(), [&](const auto& a, const auto& b) { return a_states[a.first][a.second].E > a_states[b.first][b.second].E; });
            int kept = 0, keptOf[Carriers] = {0, 0, 0};
            for (const auto& [c, j] : holes) {
                if (o.vstates > 0 ? kept >= o.vstates : keptOf[c] >= o.states) continue;
                double wall = 0, inWells = 1;
                if (choosing) measure([&, c = c, j = j](const Point& pt) { return at(pt, a_states[c][j].x); }, &wall, &inWells);
                if (!passes(wall, inWells, o.cutoffV)) {
                    rejected << QStringLiteral("%1%2").arg(QLatin1String(CarrierName[c])).arg(j + 1);
                    continue;
                }
                keepH[c][j] = 1;
                ++kept;
                ++keptOf[c];
            }
            keptH = kept;
            wantH = o.vstates > 0 ? o.vstates : (a_on[1] ? o.states : 0) + (a_on[2] ? o.states : 0);
        }
        // (Holes chosen by energy across both kinds: each kind found down to
        // them - none of either left between - else more found.)
        bool complete = true;
        if (o.vstates > 0)
            for (int c = 1; c < Carriers; ++c) {
                if (int(a_states[c].size()) < each || a_states[c].empty()) continue;
                for (std::size_t j = 0; j < keepH[1].size() + keepH[2].size(); ++j) {
                    const int kc = j < keepH[1].size() ? 1 : 2;
                    const std::size_t kj = kc == 1 ? j : j - keepH[1].size();
                    if (keepH[kc][kj] && a_states[kc][kj].E < a_states[c].back().E) complete = false;
                }
            }
        if (each >= 64 || (complete && (!choosing || (keptE >= wantE && (a_valence > 0 || keptH >= wantH))))) break;
    }

    if (a_s.axisymmetric() && !a_1d) {
        *error = tr("%1: a dot's spectrum (about an axis) is not found yet: 1D wells or 2D wires").arg(a_node->name());
        return -1;
    }
    if (!(a_activeSize > 0)) {
        *error = tr("%1: it has no wells").arg(a_node->name());
        return -1;
    }
    const bool wells = a_1d;   // 2D densities of states (free in the plane); a wire's 1D (free along it)
    if (!wells && o.normalize != 0) {
        *error = tr("%1: choosing states by their place or the walls, and an absorption per the stack or a length, are a well's (1D)")
                     .arg(a_node->name());
        return -1;
    }
    // The field across the wells: the slope of their conduction band edge
    // (a least-squares line through their points) - the uniform field's,
    // or another physics' potential's there.
    double field = std::abs(a_F);
    if (wells) {
        double sw = 0, sz = 0, se = 0, szz = 0, sze = 0;
        for (const Point& pt : a_points) {
            if (pt.d < 0 || std::size_t(pt.d) >= a_active.size() || !a_active[std::size_t(pt.d)]) continue;
            sw += pt.w;
            sz += pt.w * pt.a;
            se += pt.w * pt.pot[0];
            szz += pt.w * pt.a * pt.a;
            sze += pt.w * pt.a * pt.pot[0];
        }
        const double var = sw * szz - sz * sz;
        if (sw > 0 && var > 0) field = std::abs((sw * sze - sz * se) / var);   // eV/m: V/m
    }
    const double kT = KB * a_T, gamma = o.linewidth + o.dlinewidth * field;
    if (!wells && !(gamma > 0)) {
        *error = tr("%1: a wire's spectrum wants a broadening more than 0").arg(a_node->name());
        return -1;
    }
    if (o.excitons && !wells) {
        *error = tr("%1: excitons are found in wells (1D) only, not yet in wires").arg(a_node->name());
        return -1;
    }
    if (o.continuum == 1 && !wells) {
        *error = tr("%1: the barriers' bulk continuum is a well's (1D): a wire keeps its states").arg(a_node->name());
        return -1;
    }
    if (o.excitons && !(gamma > 0)) {
        *error = tr("%1: an exciton's line wants a broadening more than 0").arg(a_node->name());
        return -1;
    }
    a_spec = Spectrum();
    a_spec.size = a_activeSize;
    a_spec.field = field;
    a_spec.gamma = gamma;
    // What the absorption is per: the wells' thickness, the line's (the
    // stack's, its padding too: Lumerical's), or a length.
    const double per = o.normalize == 1 ? a_L : o.normalize == 2 ? o.normLength : a_activeSize;
    a_spec.per = per;
    // The photons' window, whose transitions alone are broadened where asked.
    double windowLo = Inf(), windowHi = -Inf();
    for (double E : o.energies) {
        windowLo = std::min(windowLo, E);
        windowHi = std::max(windowHi, E);
    }
    windowHi += o.windowExt;
    const bool window = o.broaden == 1;
    // (The k·p valence states, each Kramers pair: below, once found.)
    std::vector<char> keepV;
    // (k·p: the next valence pair that would pass, left out by the count -
    // its edge's depth below the last taken's at k = 0, eV, and how many
    // pairs would take it.)
    double nextVGap = NaN;
    int nextVCount = 0, nextVKept = 0;
    // The line's shape, its half width at half maximum γ: its cumulative
    // and density at x (J) from its middle.
    // (Its width at a photon's energy, gNow - the broadening grown with it,
    // linewidthslope: set in the photons' loop below; the grids the base
    // width's, the least.)
    double gNow = gamma, sigma = gamma / std::sqrt(2 * std::log(2.0)), width = gamma / std::acosh(2.0);
    auto setWidth = [&](double g) {
        gNow = g;
        sigma = g / std::sqrt(2 * std::log(2.0));
        width = g / std::acosh(2.0);
    };
    auto cdf = [&](double x) {
        if (o.lineshape == 1) return 0.5 * std::erfc(-x / (sigma * std::sqrt(2.0)));
        if (o.lineshape == 2) return x / width > 700 ? 1.0 : 2 / M_PI * std::atan(std::exp(x / width));
        return 0.5 + std::atan(x / gNow) / M_PI;
    };
    auto pdf = [&](double x) {
        if (o.lineshape == 1) return std::exp(-0.5 * x * x / (sigma * sigma)) / (sigma * std::sqrt(2 * M_PI));
        if (o.lineshape == 2) return std::abs(x / width) > 700 ? 0.0 : 1 / (M_PI * width * std::cosh(x / width));
        return gNow / M_PI / (x * x + gNow * gNow);
    };
    // The k·p valence bands: in-plane k from 0 to what the photons and the
    // holes' occupation reach - the electrons' parabola past the highest
    // photon (by 20 broadenings and 10 kT), a heavy mass's 0.15 eV and 15 kT.
    // The continuum "bound": the barriers' own gap where the line ends (a
    // field moves both edges alike), and the bound steps' fade above it -
    // cos² over o.fade, into the bulk absorption: far enough above, the
    // absorption the bulk's, the same in any field (Δn between fields
    // converges as the window widens).
    // (The barriers' lowest gap - the first, along the line, of it -, not
    // the line's start's: a well there was taken for the barriers, its steps
    // faded above its own gap - the hunt of 22 October, A1.)
    auto barrierPoint = [&]() -> const Point* {
        const Point* best = nullptr;
        double least = Inf();
        for (const Point& pt : a_points) {
            if (pt.d < 0 || (std::size_t(pt.d) < a_active.size() && a_active[std::size_t(pt.d)])) continue;
            const double gap = pt.pot[0] - std::max(-pt.pot[1], -pt.pot[2]);
            if (gap < least - 1e-9 || (gap <= least + 1e-9 && best && pt.a < best->a)) {
                least = std::min(least, gap);
                best = &pt;
            }
        }
        // (A slab of one gap - no barrier -: where the line starts, its bulk
        // whole.)
        if (!best && !a_points.empty())
            best = &*std::min_element(a_points.begin(), a_points.end(), [](const Point& a, const Point& b) { return a.a < b.a; });
        return best;
    };
    double barrierGap = NaN, barrierTop = NaN;   // (J; the barriers' valence top, eV)
    if (o.continuum == 1 && wells && !a_points.empty())
        if (const Point* end = barrierPoint()) {
            barrierTop = std::max(-end->pot[1], -end->pot[2]);
            barrierGap = (end->pot[0] - barrierTop) * Q;
        }
    const bool fading = std::isfinite(barrierGap);
    auto fadeAt = [&](double x) {
        if (!fading || x <= barrierGap) return 1.0;
        if (!(o.fade > 0) || x >= barrierGap + o.fade) return 0.0;
        const double c = std::cos(0.5 * M_PI * (x - barrierGap) / o.fade);
        return c * c;
    };
    const bool kp = a_valence > 0;
    // A k·p valence state's in-plane mass near k = 0 (m0: E = E0 - ħ²k²/2m;
    // over the directions, its inverse's mean).
    auto kpMass = [&](std::size_t j) {
        double inverse = 0;
        for (const KpBands& b : a_kps) {
            if (b.k.size() < 2 || j >= b.E[0].size()) return NaN;
            const double k1 = b.k[1] * 1e-9;
            inverse += b.weight * (b.E[0][j] - b.E[1][b.at(1, j)]) / (HB2M * k1 * k1);
        }
        return inverse > 0 ? 1 / inverse : NaN;
    };
    if (kp) {
        double topE = 0;
        for (double E : o.energies) topE = std::max(topE, E);
        double gap = Inf(), me = 1;
        if (!a_states[0].empty()) me = a_states[0].front().mPar;
        // (The states of each kind: heavy and light holes, the split-off
        // ones in the 6x6 and 8x8 - each a Kramers pair; the 8x8's electrons
        // its own, as many as the effective mass's found.)
        const int wantV = o.vstates > 0 ? o.vstates : (a_valence == 4 ? 2 : 3) * std::max(1, o.states);
        // (One pair more for the coupled excitons: the next, said if it is
        // near.)
        const int count0 = wantV + (choosing ? 4 : o.excitons && o.excitonModel == 2 ? 1 : 0);
        const int electrons = a_valence == 8 ? int(a_states[0].size()) : 0;
        // The 8x8's electrons: each state's energy its Kramers pair's at k =
        // 0, its mass in the plane their curvature's there (over the
        // directions, its inverse's mean) - not the effective mass's.
        QStringList was;
        auto kp8Electrons = [&](bool masses) {
            for (std::size_t i = 0; i < a_states[0].size(); ++i) {
                State& e = a_states[0][i];
                const KpBands& b0 = a_kps.front();
                if (b0.C.empty() || 2 * i + 1 >= b0.C[0].size()) continue;
                if (!masses) {
                    was << formatNumber(e.E, 5);
                    e.E = 0.5 * (b0.C[0][2 * i] + b0.C[0][2 * i + 1]);
                    continue;
                }
                double inverse = 0;
                for (const KpBands& b : a_kps) {
                    if (b.k.size() < 2 || 2 * i + 1 >= b.C[1].size()) {
                        inverse = NaN;
                        break;
                    }
                    const double k1 = b.k[1] * 1e-9;
                    inverse += b.weight * (0.5 * (b.C[1][2 * i] + b.C[1][2 * i + 1]) - e.E) / (HB2M * k1 * k1);
                }
                if (inverse > 0) e.mPar = 1 / inverse;
            }
        };
        // (More pairs while too few pass, as the states above.)
        int count = count0;
        for (;; count = std::min(64, 2 * count)) {
            if (!kpBands(count, 0, 1, error, electrons)) return -1;
            if (!choosing || count >= 64 || a_kps.empty() || a_kps.front().E.empty()) break;
            int passing = 0;
            const KpBands& k0 = a_kps.front();
            for (std::size_t j = 0; j < k0.E[0].size(); j += 2) {
                double wall = 0, inWells = 1;
                measure(
                    [&](const Point& pt) {
                        double v = 0;
                        for (int c = 0; c < 6; ++c) v += std::norm(at(pt, k0.x[0][j], c * a_neq));
                        return std::sqrt(v);
                    },
                    &wall, &inWells);
                passing += passes(wall, inWells, o.cutoffV) ? 1 : 0;
            }
            if (passing >= wantV) break;
        }
        if (a_valence == 8) kp8Electrons(false);
        if (!a_states[0].empty() && !a_kps.empty() && !a_kps[0].E.empty() && !a_kps[0].E[0].empty()) gap = (a_states[0].front().E - a_kps[0].E[0][0]) * Q;
        double reach = std::max(0.05 * Q, (std::isfinite(gap) ? topE - gap : 0.0) + 20 * gamma + 10 * kT);
        // (Fading, past the fade's end: the steps' tail beyond the grid none.)
        if (fading && std::isfinite(gap)) reach = std::max(reach, barrierGap + o.fade + 20 * gamma - gap);
        const double ke = std::sqrt(2 * me * M0 * reach) / Hbar, kh = std::sqrt(2 * 0.5 * M0 * (0.15 * Q + 15 * kT)) / Hbar;
        if (!kpBands(count, std::max(ke, kh), o.kpoints > 1 ? o.kpoints : 121, error, electrons)) return -1;
        const KpBands& b0 = a_kps.front();
        // The valence subbands that enter: the highest Kramers pairs (at k =
        // 0, heavy and light mixed) that pass, wantV of them.
        keepV.assign(b0.E.empty() ? 0 : b0.E[0].size(), 0);
        {
            const int nq = a_neq;
            int kept = 0;
            for (std::size_t j = 0; j < keepV.size() && kept < wantV; j += 2) {
                double wall = 0, inWells = 1;
                if (choosing)
                    measure(
                        [&](const Point& pt) {
                            double v = 0;
                            for (int c = 0; c < 6; ++c) v += std::norm(at(pt, b0.x[0][j], c * nq));
                            return std::sqrt(v);
                        },
                        &wall, &inWells);
                if (!passes(wall, inWells, o.cutoffV)) {
                    rejected << QStringLiteral("v%1").arg(j / 2 + 1);
                    continue;
                }
                for (std::size_t q = j; q < std::min(j + 2, keepV.size()); ++q) keepV[q] = 1;
                ++kept;
            }
            // (The next that would pass: the coupled excitons' warning.)
            std::size_t last = keepV.size();
            for (std::size_t j = 0; j < keepV.size(); j += 2)
                if (keepV[j]) last = j;
            // (The next in the wells, below the last taken in them: mostly
            // in them, and under "bound" above the barriers' valence top -
            // the check of 21 October's asked for one more each time,
            // chasing box states in the barriers; its count those passing
            // up to it.)
            auto inTheWells = [&](std::size_t j, double* wall, double* inWells) {
                measure(
                    [&b0, &nq, j, this](const Point& pt) {
                        double v = 0;
                        for (int c = 0; c < 6; ++c) v += std::norm(at(pt, b0.x[0][j], c * nq));
                        return std::sqrt(v);
                    },
                    wall, inWells);
                return *inWells >= 0.5 && !(std::isfinite(barrierTop) && !(b0.E[0][j] > barrierTop));
            };
            std::size_t lastIn = keepV.size();
            for (std::size_t j = 0; j < keepV.size(); j += 2) {
                double wall = 0, inWells = 1;
                if (keepV[j] && inTheWells(j, &wall, &inWells)) lastIn = j;
            }
            int between = 0;
            for (std::size_t j = last + 2; lastIn < keepV.size() && j < keepV.size(); j += 2) {
                double wall = 0, inWells = 1;
                const bool in = inTheWells(j, &wall, &inWells);
                if (choosing && !passes(wall, inWells, o.cutoffV)) continue;
                ++between;
                if (!in) continue;
                nextVGap = b0.E[0][lastIn] - b0.E[0][j];
                nextVCount = kept + between;
                nextVKept = kept;
                break;
            }
        }
        if (a_valence == 8) {
            kp8Electrons(true);
            QStringList now, masses;
            for (std::size_t i = 0; i < std::min<std::size_t>(a_states[0].size(), 4); ++i) {
                now << formatNumber(a_states[0][i].E, 5);
                masses << formatNumber(a_states[0][i].mPar, 3);
            }
            a_s.log << tr("%1: the 8x8's electrons at k = 0 %2 eV (the effective mass's %3), their masses in the plane near it %4 m0%5%6")
                           .arg(a_node->name(), now.join(QStringLiteral(", ")), was.mid(0, 4).join(QStringLiteral(", ")), masses.join(QStringLiteral(", ")),
                                a_kpRescaled.isEmpty() ? QString()
                                                       : tr("; Ep lowered where the conduction band's or the light holes' remote term "
                                                             "along the axis would be below a free electron's (Foreman's): %1")
                                                             .arg(a_kpRescaled.join(QStringLiteral(", "))),
                                kpSpuriousSaid());
        }
        QStringList top, masses;
        for (std::size_t j = 0; j < std::min<std::size_t>(b0.E[0].size(), 8); j += 2) {
            top << formatNumber(b0.E[0][j], 6);
            const double m = kpMass(j);
            masses << (std::isfinite(m) ? formatNumber(m, 3) : QStringLiteral("-"));
        }
        a_s.log << tr("%1: the %2x%2 k·p valence bands (%3) at %4 values of k to %5: at k = 0 the highest %6 eV, their masses in the "
                      "plane near it %7 m0")
                       .arg(a_node->name())
                       .arg(a_valence)
                       .arg(kpDirection())
                       .arg(b0.k.size())
                       .arg(formatQuantity(b0.k.back(), Dim::of(-1)))
                       .arg(top.join(QStringLiteral(", ")), masses.join(QStringLiteral(", ")));
    }
    if (choosing || o.cstates > 0 || o.vstates > 0) {
        int keptE = 0, keptV = 0;
        for (char k : keepE) keptE += k;
        if (kp)
            for (char k : keepV) keptV += k;
        else
            for (int c = 1; c < Carriers; ++c)
                for (char k : keepH[c]) keptV += k;
        a_s.log << tr("%1: the spectrum's subbands: %2 conduction, %3 valence%4%5")
                       .arg(a_node->name())
                       .arg(keptE)
                       .arg(kp ? keptV / 2 : keptV)
                       .arg(kp ? tr(" (Kramers pairs)") : QString())
                       .arg(rejected.isEmpty() ? QString() : tr("; %1 left out (at a wall, or out of the wells)").arg(rejected.join(QStringLiteral(", "))));
    }
    // Carriers of carrier c per area (wells) or length (wires) with a
    // quasi-Fermi level F (J).
    auto sheet = [&](int c, double F) {
        if (a_valence == 8 && c == 0) {
            // The 8x8's electrons (each state one: Kramers' pairs both), as
            // the holes are counted (the grid's reach, the holes' at 0.5 m0,
            // far past them).
            double nn = 0;
            for (const KpBands& b : a_kps)
                for (std::size_t i = 0; i < (b.C.empty() ? 0 : b.C[0].size()); ++i) {
                    if (i / 2 < keepE.size() && !keepE[i / 2]) continue;
                    for (std::size_t m = 1; m < b.k.size(); ++m) {
                        const double Ea = b.C[m - 1][b.atC(m - 1, i)] * Q, Eb = b.C[m][b.atC(m, i)] * Q, dk2 = b.k[m] * b.k[m] - b.k[m - 1] * b.k[m - 1];
                        const double in = std::abs(Eb - Ea) > 1e-6 * kT ? kT * (softplus((F - Ea) / kT) - softplus((F - Eb) / kT)) / (Eb - Ea)
                                                                       : fermi((0.5 * (Ea + Eb) - F) / kT);
                        nn += b.weight * dk2 / (4 * M_PI) * in;
                    }
                }
            return nn;
        }
        if (kp && c > 0) {
            // The holes of the k·p bands (each state one: Kramers' pairs both),
            // each interval's energy linear in k²: ∫ (1 - f) dk² = Δk²/ΔE kT
            // [ln(1 + e^((E - F)/kT))] - a parabolic band's exactly.
            if (c == 2) return 0.0;
            auto softplus = [](double x) { return x > 0 ? x + std::log1p(std::exp(-x)) : std::log1p(std::exp(x)); };
            double p = 0;
            for (const KpBands& b : a_kps)
                for (std::size_t j = 0; j < (b.E.empty() ? 0 : b.E[0].size()); ++j) {
                    if (j < keepV.size() && !keepV[j]) continue;
                    for (std::size_t m = 1; m < b.k.size(); ++m) {
                        const double Ea = b.E[m - 1][b.at(m - 1, j)] * Q, Eb = b.E[m][b.at(m, j)] * Q, dk2 = b.k[m] * b.k[m] - b.k[m - 1] * b.k[m - 1];
                        const double holes = std::abs(Eb - Ea) > 1e-6 * kT ? kT * (softplus((Eb - F) / kT) - softplus((Ea - F) / kT)) / (Eb - Ea)
                                                                          : 1 - fermi((0.5 * (Ea + Eb) - F) / kT);
                        p += b.weight * dk2 / (4 * M_PI) * holes;
                    }
                }
            return p;
        }
        double n = 0;
        for (std::size_t i = 0; i < a_states[c].size(); ++i) {
            if (!(c == 0 ? keepE[i] : keepH[c][i])) continue;
            const State& s = a_states[c][i];
            const double m = s.mPar * M0, E = s.E * Q;
            const double x = c == 0 ? (F - E) / kT : (E - F) / kT;
            if (wells) n += m * kT / (M_PI * Hbar * Hbar) * softplus(x);
            else {
                // ∫ (1/πħ) √(2m/ε) f dε = (2√(2m)/πħ) ∫ f(u²) du.
                const int steps = 400;
                const double top = std::sqrt(std::max(40.0, x + 40) * kT);
                double sum = 0;
                for (int i = 0; i < steps; ++i) {
                    const double u = (i + 0.5) * top / steps;
                    sum += fermi(u * u / kT - x) * top / steps;
                }
                n += 2 * std::sqrt(2 * m) / (M_PI * Hbar) * sum;
            }
        }
        return n;
    };
    a_spec.carriers = o.density > 0;
    if (a_spec.carriers) {
        const double want = o.density * a_activeSize;
        auto level = [&](bool electrons) {
            double lo = -30 * Q, hi = 30 * Q;
            for (int it = 0; it < 200; ++it) {
                const double mid = (lo + hi) / 2;
                const double have = electrons ? sheet(0, mid) : sheet(1, mid) + sheet(2, mid);
                // (Electrons' count rises with F, holes' falls.)
                if ((have < want) == electrons) lo = mid;
                else hi = mid;
            }
            return (lo + hi) / 2;
        };
        a_spec.Fc = level(true);
        a_spec.Fv = level(false);
        if (!a_states[0].empty() && a_spec.Fc > a_states[0].back().E * Q - 3 * kT)
            a_s.log << tr("Warning: %1: the electrons' quasi-Fermi level is near the highest state found: ask for more states").arg(a_node->name());
    }
    // The continuum "bound": a state confined by its own well - below
    // (a hole's above) the barriers' band edge just outside the wells'
    // stretch its mean position is in, the lower (higher) side's. A field
    // lowers the line's far ends: a state above them but below its well's
    // own barriers is quasi-bound, tunnelling out through the triangle only.
    struct Stretch {
        double lo = 0, hi = 0;
        double edge[Carriers] = {Inf(), -Inf(), -Inf()};
        double far[Carriers] = {-Inf(), Inf(), Inf()};   // (its higher side's: in a field the other)
        bool walled = false;   // a barrier one side or both: a well (not a slab of one gap)
        double size = 0;       // its thickness (m)
        int domain = -1;       // its middle's
        double gap = NaN;      // its own gap there (J)
    };
    std::vector<Stretch> stretches;
    // (The continuum "states" with the bulk's excitons: the barriers' Elliott
    // edge less their free carriers', the box's states the free continuum
    // already - n = 3 above a superlattice's barriers and the barriers' own
    // excitonic edge in one run, the update check of 21 October, 3.)
    const bool barrierExcitons = o.continuum == 0 && wells && o.bulkExcitons && !a_points.empty();
    if ((o.continuum == 1 || barrierExcitons) && wells) {
        std::vector<const Point*> along;
        for (const Point& pt : a_points)
            if (pt.d >= 0) along.push_back(&pt);
        std::sort(along.begin(), along.end(), [](const Point* a, const Point* b) { return a->a < b->a; });
        auto active = [&](const Point* pt) { return std::size_t(pt->d) < a_active.size() && a_active[std::size_t(pt->d)]; };
        for (std::size_t i = 0; i < along.size();) {
            if (!active(along[i])) {
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j + 1 < along.size() && active(along[j + 1])) ++j;
            Stretch st;
            st.lo = along[i]->a;
            st.hi = along[j]->a;
            for (std::size_t k = i; k <= j; ++k) st.size += along[k]->w * 1e-9;
            const Point* middle = along[(i + j) / 2];
            st.domain = middle->d;
            st.gap = (middle->pot[0] - std::max(-middle->pot[1], -middle->pot[2])) * Q;
            int sides = 0;
            for (const Point* side : {i > 0 ? along[i - 1] : nullptr, j + 1 < along.size() ? along[j + 1] : nullptr}) {
                if (!side) continue;
                ++sides;
                st.edge[0] = std::min(st.edge[0], side->pot[0]);
                st.far[0] = std::max(st.far[0], side->pot[0]);
                for (int c = 1; c < Carriers; ++c) {
                    st.edge[c] = std::max(st.edge[c], -side->pot[c]);
                    st.far[c] = std::min(st.far[c], -side->pot[c]);
                }
            }
            st.walled = sides > 0;
            // (No barrier either side - a slab of one gap: nothing confined.)
            if (sides == 0) {
                st.edge[0] = -Inf();
                for (int c = 1; c < Carriers; ++c) st.edge[c] = Inf();
            }
            stretches.push_back(st);
            i = j + 1;
        }
    }
    auto nearest = [&](double pos) {
        const Stretch* best = &stretches.front();
        for (const Stretch& st : stretches) {
            auto distance = [&](const Stretch& x) { return pos < x.lo ? x.lo - pos : pos > x.hi ? pos - x.hi : 0.0; };
            if (distance(st) < distance(*best)) best = &st;
        }
        return best;
    };
    auto confined = [&](int c, double E, double pos) {
        if (stretches.empty()) return true;
        const Stretch* best = nearest(pos);
        // (Carrier 3: the k·p holes, mixed - the valence band's top.)
        return c == 0 ? E < best->edge[0] : E > (c == 3 ? std::max(best->edge[1], best->edge[2]) : best->edge[c]);
    };
    // In a field, a state is bound also when its well holds it: tilted past
    // its lower barrier's edge, below its higher one's, and "localized" of
    // its |ψ|² there (select's) - it was left out with its strength, not
    // given back by the bulk's (the update check of 21 October, 7a). Above
    // both, a state the box's continuum, held or not.
    auto held = [&](int c, const State& st) {
        if (stretches.empty() || a_F == 0) return false;
        const Stretch* best = nearest(st.pos);
        // (A slab of one gap - no barrier, no edges - holds nothing.)
        if (!(c == 0 ? st.E < best->far[0] : st.E > (c == 3 ? std::max(best->far[1], best->far[2]) : best->far[c]))) return false;
        const double all = integral(st.x, st.x, {});
        const double in = integral(st.x, st.x, [best](int, double a) { return a >= best->lo && a <= best->hi ? 1.0 : 0.0; });
        return all > 0 && in / all >= o.localized;
    };
    auto bound = [&](int c, const State& st) { return confined(c, st.E, st.pos) || held(c, st); };
    // (Said: the electrons a field tilts past their barriers' edges whose
    // wells hold them - bound, their pairs taken.)
    if (o.continuum == 1 && a_F != 0) {
        // (Each with its share in its well, and the barriers' own count: the
        // states' count said "0 bound" before it, and nothing said how much
        // "pmin" decides - the spectrum 5 times apart at 0.5 and 0.9: the
        // hunt of 22 October, N16.)
        QStringList heldOnes, near;
        int barriers = 0;
        for (std::size_t i = 0; i < a_states[0].size(); ++i) {
            const State& st = a_states[0][i];
            if (confined(0, st.E, st.pos)) {
                ++barriers;
                continue;
            }
            if (stretches.empty()) continue;
            const Stretch* best = nearest(st.pos);
            if (!(st.E < best->far[0])) continue;
            const double all = integral(st.x, st.x, {});
            const double in = integral(st.x, st.x, [best](int, double a) { return a >= best->lo && a <= best->hi ? 1.0 : 0.0; });
            if (!(all > 0)) continue;
            const QString one = QStringLiteral("e%1 (%2)").arg(i + 1).arg(formatNumber(in / all, 2));
            if (in / all >= o.localized) heldOnes << one;
            else near << one;
        }
        if (!heldOnes.isEmpty() || !near.isEmpty())
            a_s.log << tr("%1: in this field past their lower barrier's edge and below their higher one's, so bound where their well holds "
                          "%2 of their |ψ|² at least (\"pmin\"), beside the %3 the barriers bind: %4%5. Which are taken in hangs on \"pmin\" "
                          "between their shares, and the absorption with it")
                           .arg(a_node->name(), formatNumber(o.localized, 3))
                           .arg(barriers)
                           .arg(heldOnes.isEmpty() ? tr("none held") : tr("held %1").arg(heldOnes.join(QLatin1String(", "))))
                           .arg(near.isEmpty() ? QString() : tr("; not held %1").arg(near.join(QLatin1String(", "))));
    }
    // The transitions e_i to hh_j and lh_j: their overlaps, polarizations
    // (at the band edge: TE 3/2 and 1/2 of |Mb|², TM 0 and 2) and masses.
    struct Transition {
        double E, Ee, Eh;   // J
        double overlap;
        double te, tm;
        double me, mh, mr;   // kg
        double Ep, nr;
        double Eb = 0, radius = NaN, F0 = NaN;   // its 1s exciton's (F0: |F(0)|²)
        bool e1hh1 = false;
        // Its states (c: its hole's kind), its overlap signed; coupled: its
        // line and continuum the coupled subbands' excitons'.
        int ie = -1, ih = -1, c = 0;
        double ov = 0;
        bool coupled = false;
    };
    // The coupled subbands' excitons (excitonmodel coupled): each block's
    // states and strengths, its components TE's and TM's, its spin pairs.
    struct CoupledBlock {
        exciton::Result r;
        double spin = 1;
        std::vector<int> te, tm;
        // Its least pair's edge; each state's cells - a state above the edge
        // its continuum's, spread over its cells (its largest parts' pairs'
        // shares of the plane at their k points: as the free continuum's
        // cells, smooth however far apart its states, each pair's continuum
        // its own where they interleave), one below it a line (none).
        double edge = 0;
        std::vector<std::vector<exciton::Result::Cell>> spread;
        void cells()
        {
            const std::size_t n = std::size_t(std::lower_bound(r.energy.begin(), r.energy.end(), r.cut) - r.energy.begin());
            spread.assign(n, {});
            for (std::size_t i = 0; i < n && i < r.cells.size(); ++i)
                if (r.energy[i] >= edge) spread[i] = r.cells[i];
        }
    };
    std::vector<CoupledBlock> xBlocks;
    // (The points along the axis in order: the envelopes' at them, each by
    // √w - a density between two states their product.)
    std::vector<std::size_t> xOrder(a_points.size());
    std::iota(xOrder.begin(), xOrder.end(), std::size_t(0));
    std::sort(xOrder.begin(), xOrder.end(), [this](std::size_t a, std::size_t b) { return a_points[a].a < a_points[b].a; });
    auto xEnvelopes = [this, &xOrder] {
        exciton::Envelopes env;
        for (std::size_t p : xOrder) env.z.push_back(a_points[p].a);
        return env;
    };
    // (Along the axis, ħ²/2m at the points: the electrons', and a hole's -
    // parabolic, its kind's (1 heavy, 2 light); k·p (0), each spinor
    // component's kz² coefficient, e± γ1 - 2γ2 and Z γ1 + 4γ2, wurtzite's
    // -(A1 + A3) and -A1.)
    auto xStiffness = [this, &xOrder](exciton::Envelopes& env, int kind) {
        const double hb = Hbar * Hbar / (2 * M0);
        env.electronStiffness.clear();
        env.holeStiffness.assign(kind > 0 ? 1 : 6, {});
        for (std::size_t p : xOrder) {
            const Band& b = a_bands[std::size_t(std::max(0, a_points[p].d))];
            env.electronStiffness.push_back(b.me > 0 ? hb / b.me : 0.0);
            if (kind > 0) {
                const double m = kind == 1 ? b.mhhZ : b.mlhZ;
                env.holeStiffness[0].push_back(m > 0 ? hb / m : 0.0);
                continue;
            }
            const double xy = b.wz ? -(b.A[0] + b.A[2]) * hb : (b.g1 - 2 * b.g2) * hb, zz = b.wz ? -b.A[0] * hb : (b.g1 + 4 * b.g2) * hb;
            for (int c = 0; c < 6; ++c) env.holeStiffness[std::size_t(c)].push_back(c == 1 || c == 4 ? zz : xy);
        }
    };
    auto xEnvelope = [this, &xOrder](const Eigen::VectorXd& x) {
        std::vector<double> v;
        v.reserve(xOrder.size());
        for (std::size_t p : xOrder) v.push_back(std::sqrt(a_points[p].w) * at(a_points[p], x));
        return v;
    };
    // (Its continuum to the window's top and 30 line widths past it, 0.3 eV
    // at least - not 30 kT: excitons have no carriers, and that took it to
    // 0.78 eV at 300 K -; its states half the line's width apart there, 0.5
    // to 3 meV - each spread over its cells - "excitonsize" of them at most,
    // 1200: a second's solve.)
    const int xSize = o.excitonSize > 0 ? o.excitonSize : 1200;
    const bool drawn = o.excitonDrawn;
    auto xOptions = [gamma, windowHi, xSize, drawn](double epsr, int components, double lowest) {
        exciton::Options xo;
        xo.epsr = epsr;
        xo.components = components;
        xo.correlated = drawn ? 4 : 0;
        xo.reach = std::max({60 * gamma, 0.3 * Q, windowHi - lowest + 30 * gamma});
        xo.spacing = std::clamp(gamma / 2, 0.5e-3 * Q, 3e-3 * Q);
        xo.maxSize = xSize;
        return xo;
    };
    // A block's continuum widened to fit its size: the widest said once, in
    // a warning, what would smooth it with it.
    struct Widened {
        double spacing = 0, asked = 0;
        int pairs = 0, points = 0, size = 0;
    } widest;
    // (The pairs drawn together, said once: how many, and β.)
    bool drawnSaid = false;
    int droppedDirections = 0;   // (k points whose functions drawn together were left out: fem_exciton's Result::dropped)
    auto xDrawn = [this, &drawnSaid, &droppedDirections](const CoupledBlock& blk) {
        droppedDirections += blk.r.dropped;
        if (drawnSaid || blk.r.correlated <= 0) return;
        drawnSaid = true;
        const QString g = formatQuantity(1 / blk.r.beta, Dim::length());
        a_s.log << (blk.r.correlated == 1 ? tr("%1: the coupled excitons' pair drawn together along the axis, e^(-|ze - zh|/%2)").arg(a_node->name(), g)
                                          : tr("%1: the coupled excitons' lowest %2 pairs drawn together along the axis, e^(-|ze - zh|/%3)")
                                                .arg(a_node->name())
                                                .arg(blk.r.correlated)
                                                .arg(g));
    };
    auto xSpacing = [&widest](const CoupledBlock& blk, int pairs, int size) {
        if (!(blk.r.spacing > blk.r.asked * (1 + 1e-9)) || !(blk.r.spacing / blk.r.asked > widest.spacing / std::max(widest.asked, 1e-300))) return;
        widest = {blk.r.spacing, blk.r.asked, pairs, blk.r.points, size};
    };
    // The e1-hh1 exciton's globals of its blocks: its edge less its
    // strongest TE line below it, the radius of Bastard's 1s of that
    // |ψ(0)|² (λ: 2/(πλ²)) - \a weight the pair's TE amplitudes' |·|²
    // summed.
    auto xGlobals = [this](const std::vector<const CoupledBlock*>& blocks, double edge, double weight) {
        double best = 0, at = NaN;
        for (const CoupledBlock* blk : blocks)
            for (std::size_t n = 0; n < blk->r.energy.size() && blk->r.energy[n] < edge; ++n) {
                double sum = 0;
                for (int c : blk->te) sum += blk->r.strength[std::size_t(c)][n];
                if (sum > best) {
                    best = sum;
                    at = blk->r.energy[n];
                }
            }
        if (!std::isfinite(at)) return;
        a_spec.Ex = at;
        a_spec.Eb = edge - a_spec.Ex;
        a_spec.radius = weight > 0 ? std::sqrt(2 / (M_PI * best / weight)) : NaN;
        a_spec.gammaX = NaN;
    };
    std::vector<Transition> transitions;
    // (Each electron's "bound" once: held() integrates.)
    std::vector<char> boundOf[Carriers];
    if (o.continuum == 1)
        for (const State& st : a_states[0]) boundOf[0].push_back(bound(0, st));
    for (std::size_t i = 0; i < a_states[0].size(); ++i) {
        if (!keepE[i]) continue;
        const State& e = a_states[0][i];
        for (int c = 1; c < Carriers; ++c)
            for (std::size_t j = 0; j < a_states[c].size(); ++j) {
                if (!keepH[c][j]) continue;
                const State& h = a_states[c][j];
                Transition t;
                t.Ee = e.E * Q;
                t.Eh = h.E * Q;
                t.E = t.Ee - t.Eh;
                const double ov = integral(e.x, h.x, {});
                t.overlap = ov * ov;
                t.ov = ov;
                t.ie = int(i);
                t.ih = int(j);
                t.c = c;
                t.te = c == 1 ? 1.5 : 0.5;
                // (TM by Kane's Ep along the axis: wurtzite's Ep∥.)
                t.tm = (c == 1 ? 0.0 : 2.0) * (e.Ep > 0 ? e.EpZ / e.Ep : 1.0);
                t.me = e.mPar * M0;
                t.mh = h.mPar * M0;
                t.mr = 1 / (1 / t.me + 1 / t.mh);
                t.Ep = e.Ep * Q;
                t.nr = o.index > 0 ? o.index : e.nr;
                t.e1hh1 = i == 0 && c == 1 && j == 0;
                // (The continuum the barriers' bulk absorption: an unbound
                // electron's pairs its. A bound electron's with every hole -
                // its strength theirs whatever the field, Σ|<e|h>|² 1 over
                // them: in a field it moves to holes the field frees, and was
                // lost with them - 11 % of a well's TM absorption at 150
                // kV/cm, the update check of 21 October, 7a. Each electron
                // once, the sum kept; a bound hole's with unbound electrons
                // too would count again what the bulk has.)
                if (o.continuum == 1 && !boundOf[0][i]) continue;
                // (Coupled: a dark pair too - an indirect exciton, across a
                // barrier, bound and mixing with the bright.)
                if (t.overlap > 1e-10 || (o.excitons && o.excitonModel == 2)) transitions.push_back(t);
            }
    }
    a_spec.nr = o.index > 0 ? o.index : (a_states[0].empty() ? 3.5 : a_states[0].front().nr);
    if (kp) transitions.clear();   // (the k·p bands' below)
    // Excitons: each transition's 1s binding (in the wells' permittivity);
    // screened by carriers, none.
    a_spec.excitons = o.excitons && !a_spec.carriers;
    // (The 8x8's: each pair's own 1s, its electron the 8x8's state - the
    // coupled subbands' spinors the 4x4's and 6x6's alone: said.)
    if (a_spec.excitons && a_valence == 8 && o.excitonModel == 2)
        a_s.log << tr("Warning: %1: the 8x8 bands' excitons each pair's own 1s (two parameters, its electron the 8x8's state): the coupled "
                      "subbands' take the 4x4's and 6x6's spinors alone")
                       .arg(a_node->name());
    if (o.excitons && a_spec.carriers)
        a_s.log << tr("%1: the excitons are left out: carriers in the wells screen them").arg(a_node->name());
    if (a_spec.excitons) {
        double epsr = o.epsr;
        if (!(epsr > 0))
            for (std::size_t d = 0; d < a_bands.size() && !(epsr > 0); ++d)
                if (d < a_active.size() && a_active[d] && a_bands[d].on) epsr = a_bands[d].epsr;
        if (!(epsr > 0)) {
            *error = tr("%1: the excitons want the wells' permittivity: give their material's epsilonr, or the step's").arg(a_node->name());
            return -1;
        }
        // (Each transition's states, found again by their energies.)
        auto find = [&](int c, double E) -> const State* {
            for (const State& st : a_states[c])
                if (st.E * Q == E) return &st;
            return nullptr;
        };
        // (Transitions all but forbidden: |<e|h>|² 1e-4 or more - the
        // others' lines and continua next to nothing.)
        for (Transition& t : transitions) {
            if (o.excitonModel == 2) break;   // (the coupled subbands': below)
            if (t.overlap < 1e-4 && !t.e1hh1) continue;
            const int c = t.tm > 0 ? 2 : 1;
            const State* e = find(0, t.Ee);
            const State* h = find(c, t.Eh);
            if (!e || !h) continue;
            const Exciton x = exciton(*e, *h, c, t.mr, epsr, o.excitonModel == 0);
            t.Eb = x.Eb;
            t.radius = x.radius;
            t.F0 = x.F0;
            if (t.e1hh1) {
                a_spec.Eb = x.Eb;
                a_spec.Ex = t.E - x.Eb;
                a_spec.radius = x.radius;
                a_spec.gammaX = x.gamma;
            }
        }
        // The subbands' excitons coupled (fem_exciton): each kind of hole's
        // pairs one Hamiltonian in k - heavy and light apart, their Bloch
        // functions orthogonal: the attraction couples none of one with the
        // other's -, the lowest 12 of each by energy; the rest free steps.
        if (o.excitonModel == 2 && !kp)
            for (int c = 1; c < Carriers; ++c) {
                std::vector<Transition*> mine;
                for (Transition& t : transitions)
                    if (t.c == c) mine.push_back(&t);
                std::sort(mine.begin(), mine.end(), [](const Transition* a, const Transition* b) { return a->E < b->E; });
                const std::size_t most = o.excitonPairs > 0 ? std::size_t(o.excitonPairs) : 12;
                if (mine.size() > most) {
                    a_s.log << tr("%1: the %2 lowest of the %3 pairs of %4 coupled, the others free steps")
                                   .arg(a_node->name())
                                   .arg(most)
                                   .arg(mine.size())
                                   .arg(c == 1 ? tr("heavy holes") : tr("light holes"));
                    mine.resize(most);
                }
                if (mine.empty()) continue;
                exciton::Envelopes env = xEnvelopes();
                std::map<int, int> eIndex, hIndex;
                std::vector<exciton::Pair> xp;
                for (Transition* t : mine) {
                    if (!eIndex.count(t->ie)) {
                        eIndex[t->ie] = int(env.electrons.size());
                        env.electrons.push_back(xEnvelope(a_states[0][std::size_t(t->ie)].x));
                    }
                    if (!hIndex.count(t->ih)) {
                        hIndex[t->ih] = int(env.holes.size());
                        const std::vector<double> h = xEnvelope(a_states[c][std::size_t(t->ih)].x);
                        env.holes.push_back({std::vector<std::complex<double>>(h.begin(), h.end())});
                    }
                    exciton::Pair x;
                    x.electron = eIndex[t->ie];
                    x.hole = hIndex[t->ih];
                    const double E0 = t->E, mr = t->mr;
                    x.energy = [E0, mr](double k) { return E0 + Hbar * Hbar * k * k / (2 * mr); };
                    // (Its amplitudes, TE's and TM's: |·|² its overlap, its Kane
                    // energy, its polarization's weight, over the index.)
                    const double aTE = t->ov * std::sqrt(t->Ep * t->te / t->nr), aTM = t->ov * std::sqrt(t->Ep * t->tm / t->nr);
                    x.amplitude = [aTE, aTM](double, std::vector<std::complex<double>>& a) {
                        a[0] = aTE;
                        a[1] = aTM;
                    };
                    xp.push_back(std::move(x));
                    t->coupled = true;
                }
                xStiffness(env, c);
                CoupledBlock blk;
                blk.r = exciton::solve(env, xp, xOptions(epsr, 2, mine.front()->E));
                if (blk.r.energy.empty()) {
                    *error = tr("%1: the coupled excitons: %2").arg(a_node->name(), blk.r.note);
                    return -1;
                }
                blk.spin = 2;
                blk.te = {0};
                blk.tm = {1};
                blk.edge = mine.front()->E;
                blk.cells();
                xSpacing(blk, int(mine.size()), xSize);
                xDrawn(blk);
                // The e1-hh1's: its edge less the strongest line below it.
                for (Transition* t : mine)
                    if (t->e1hh1) xGlobals({&blk}, t->E, t->overlap * t->Ep * t->te / t->nr);
                xBlocks.push_back(std::move(blk));
            }
    }
    // The 2D Sommerfeld factor of a transition's continuum ε above its edge:
    // 2/(1 + e^(-2π √(R/ε))), R a quarter of its exciton's binding (the
    // strictly 2D exciton's Rydberg).
    auto sommerfeld = [](const Transition& t, double eps) {
        if (!(t.Eb > 0)) return 1.0;
        if (!(eps > 0)) return 2.0;
        return 2 / (1 + std::exp(-2 * M_PI * std::sqrt(t.Eb / 4 / eps)));
    };
    // Each transition's density of states broadened (by the line's shape,
    // half width γ) and weighed by f_v - f_c along its in-plane dispersion
    // (with carriers, f_c (1 - f_v) too: the spontaneous emission's): in
    // cells, the line integrated exactly over each.
    const double top = std::max(60 * gamma, std::max(0.3 * Q, 30 * kT));
    const double step = std::max(1e-6 * Q, std::min(gamma > 0 ? gamma / 6 : kT / 6, kT / 6));
    const int cells = std::clamp(int(std::ceil(top / step)), 50, 20000);
    auto occupation = [&](const Transition& t, double eps) {
        if (!a_spec.carriers) return 1.0;
        const double Ec = t.Ee + t.mr / t.me * eps, Ev = t.Eh - t.mr / t.mh * eps;
        return fermi((Ev - a_spec.Fv) / kT) - fermi((Ec - a_spec.Fc) / kT);
    };
    auto emission = [&](const Transition& t, double eps) {
        if (!a_spec.carriers) return 0.0;
        const double Ec = t.Ee + t.mr / t.me * eps, Ev = t.Eh - t.mr / t.mh * eps;
        return fermi((Ec - a_spec.Fc) / kT) * (1 - fermi((Ev - a_spec.Fv) / kT));
    };
    auto line = [&](double a, double b, double delta) {
        // ∫_a^b L(delta - ε) dε, the line's shape L.
        return cdf(b - delta) - cdf(a - delta);
    };
    // Broadened within the photons' window alone: whether an energy is in it,
    // and a cell's energies [a, b] cut to it (\a part: the share left; false:
    // none of it).
    auto inWindow = [windowLo, windowHi](double e) { return e >= windowLo && e <= windowHi; };
    auto clip = [windowLo, windowHi](double& a, double& b, double* part) {
        const double lo = std::min(a, b), hi = std::max(a, b);
        if (!(hi - lo > 1e-9 * Q)) {
            *part = 1;
            return lo >= windowLo && lo <= windowHi;
        }
        const double cl = std::max(lo, windowLo), ch = std::min(hi, windowHi);
        if (!(ch > cl)) return false;
        *part = (ch - cl) / (hi - lo);
        a = cl;
        b = ch;
        return true;
    };
    // The bulk absorption (continuum bound), per the wells' thickness as the
    // states' was: the barriers' own over their thickness - the line less
    // its walled wells -, and the wells' own over theirs, fading in as their
    // bound steps fade out (the first well's material for all); a slab of one
    // gap (no barrier, no well) its bulk whole. Each the 3D densities of
    // states of its heavy and light holes with its electrons.
    struct Bulk {
        double Eg = NaN, mrHH = 0, mrLH = 0, Ep = 0, nr = 3.5, weight = 0;
        double epsr = 12.9;   // (its excitons')
        bool fadesIn = false;
        bool excess = false;  // Elliott's less the free carriers': the continuum "states"
    };
    std::vector<Bulk> bulks;
    if (fading || barrierExcitons) {
        auto of = [&](const Band& b, double gap, double weight, bool fadesIn) {
            Bulk k;
            const double mhh = std::cbrt(b.mhhZ * b.mhhP * b.mhhP), mlh = std::cbrt(b.mlhZ * b.mlhP * b.mlhP);
            k.Eg = gap;
            k.mrHH = 1 / (1 / b.me + 1 / mhh) * M0;
            k.mrLH = 1 / (1 / b.me + 1 / mlh) * M0;
            k.Ep = b.Ep * Q;
            k.nr = o.index > 0 ? o.index : (std::isfinite(b.nr) ? b.nr : 3.5);
            k.epsr = o.epsr > 0 ? o.epsr : std::isfinite(b.epsr) && b.epsr > 0 ? b.epsr : k.nr * k.nr;
            k.weight = weight;
            k.fadesIn = fadesIn;
            return k;
        };
        double walled = 0;
        const Stretch* well = nullptr;
        for (const Stretch& st : stretches)
            if (st.walled) {
                walled += st.size;
                if (!well) well = &st;
            }
        // The barriers: each run of one material outside the wells its own
        // bulk - its gap, masses and Rydberg - over its own thickness. (The
        // line's start's for all: a well there gave its own excitons over the
        // barriers' thickness, and barriers of two alloys were 11 times
        // apart mirrored - the hunt of 22 October, A1.)
        struct Run {
            int d = -1;
            double size = 0, gap = NaN;
        };
        std::vector<Run> runs;
        {
            std::vector<const Point*> line;
            for (const Point& pt : a_points)
                if (pt.d >= 0) line.push_back(&pt);
            std::sort(line.begin(), line.end(), [](const Point* a, const Point* b) { return a->a < b->a; });
            for (const Point* pt : line) {
                if (std::size_t(pt->d) < a_active.size() && a_active[std::size_t(pt->d)]) {
                    runs.push_back(Run());   // (a well between: a run ends)
                    continue;
                }
                if (runs.empty() || runs.back().d != pt->d) runs.push_back(Run{pt->d, 0, NaN});
                runs.back().size += pt->w * 1e-9;
                const double gap = (pt->pot[0] - std::max(-pt->pot[1], -pt->pot[2])) * Q;
                if (!std::isfinite(runs.back().gap)) runs.back().gap = gap;
            }
        }
        // (Runs of one material together: its bulk once, their thickness.)
        std::map<int, Run> byMaterial;
        for (const Run& r : runs) {
            if (r.d < 0 || !(r.size > 0) || !std::isfinite(r.gap)) continue;
            Run& m = byMaterial[r.d];
            if (m.d < 0) m = r;
            else m.size += r.size;
        }
        // (Their thicknesses the barriers' whole, the line's less its walled
        // wells, as before: the points' weights alone missed the line's ends.)
        double counted = 0;
        for (const auto& [d, r] : byMaterial) counted += r.size;
        const double scale = counted > 0 && a_L - walled > 0 ? (a_L - walled) / counted : 1.0;
        for (const auto& [d, r] : byMaterial) {
            bulks.push_back(of(a_bands[std::size_t(d)], r.gap, r.size * scale / per, false));
            bulks.back().excess = !fading;
        }
        // (A slab of one gap - no barrier, no well -: its bulk whole.)
        if (byMaterial.empty() && a_L - walled > 0)
            if (const Point* end = barrierPoint(); end && end->d >= 0) {
                const double gap = (end->pot[0] - std::max(-end->pot[1], -end->pot[2])) * Q;
                if (std::isfinite(gap)) {
                    bulks.push_back(of(a_bands[std::size_t(end->d)], gap, (a_L - walled) / per, false));
                    bulks.back().excess = !fading;
                }
            }
        if (fading && well && well->domain >= 0 && std::isfinite(well->gap))
            bulks.push_back(of(a_bands[std::size_t(well->domain)], well->gap, walled / per, true));
    }
    // The k·p bands' transitions: each electron state with each valence
    // state at each k of each direction - its energy (J), its TE and TM
    // weights in |Mb|² (P² = 3 |Mb|²: TE (3/2) Σs |<e|Xs>|² + |<e|Ys>|², TM 3
    // Σs |<e|Zs>|², the electron's spin s each), the electron's and hole's
    // energies; with excitons, its 1s binding of the k = 0 states (alike in
    // every direction; the hole's in-plane mass of its curvature there).
    struct KpPair {
        const KpBands* bands = nullptr;
        double weight = 1;   // its direction's share
        std::vector<double> eps, wte, wtm, ec, ev;
        double Eb = 0, radius = NaN, F0 = NaN, Ep = 0, nr = 3.5;
        double edge = 0;   // its least energy (J)
        // Its electron and valence states; coupled: the coupled subbands'
        // excitons' (4x4, 6x6).
        int ie = -1, jv = -1;
        bool coupled = false;
    };
    std::vector<KpPair> pairs;
    if (kp && !a_kps.empty() && !a_kps[0].E.empty()) {
        const int n = a_neq;
        double epsr = o.epsr;
        if (a_spec.excitons && !(epsr > 0))
            for (std::size_t d = 0; d < a_bands.size() && !(epsr > 0); ++d)
                if (d < a_active.size() && a_active[d] && a_bands[d].on) epsr = a_bands[d].epsr;
        if (a_valence == 8 && !a_states[0].empty()) {
            // The 8x8's: each conduction state (its own spin) with each
            // valence state at each k - <c|p_α|v> = (i m0 P/ħ) Σs (<c_Ss|v_αs>
            // - <c_αs|v_Ss>), TE (3/2)(|A_X|² + |A_Y|²) and TM 3 |A_Z|² of
            // |Mb|² (a pure S↑ and heavy hole 3/2 and 0, as the 4x4's); the
            // momentum's Ep the material's as given, the electron's.
            std::vector<Eigen::Triplet<double>> tm;
            for (const Point& pt : a_points)
                for (int q = 0; q < pt.n; ++q)
                    for (int r = 0; r < pt.n; ++r)
                        if (pt.eq[q] >= 0 && pt.eq[r] >= 0) tm.emplace_back(pt.eq[q], pt.eq[r], pt.w * pt.N[q] * pt.N[r]);
            Eigen::SparseMatrix<double> Mn(n, n);
            Mn.setFromTriplets(tm.begin(), tm.end());
            const Eigen::SparseMatrix<std::complex<double>> Mnc = Mn.cast<std::complex<double>>();
            // (Each pair's 1s - its electron's level and its valence pair's,
            // at k = 0 - once.)
            std::map<std::pair<int, int>, Exciton> x8;
            for (const KpBands& b : a_kps) {
                if (b.C.empty()) continue;
                const std::size_t nc = b.C[0].size();
                for (std::size_t j = 0; j < b.E[0].size(); ++j) {
                    if (j < keepV.size() && !keepV[j]) continue;
                    std::vector<KpPair> prs(nc);
                    for (std::size_t i = 0; i < nc; ++i) {
                        const State& e = a_states[0][std::min(i / 2, a_states[0].size() - 1)];
                        prs[i].bands = &b;
                        prs[i].weight = b.weight;
                        prs[i].Ep = e.Ep * Q;
                        prs[i].nr = o.index > 0 ? o.index : e.nr;
                    }
                    for (std::size_t m = 0; m < b.k.size(); ++m) {
                        if (j >= b.E[m].size() || nc > b.C[m].size()) break;
                        // (M times the valence state's parts, once for every
                        // conduction state.)
                        Eigen::VectorXcd mv[8];
                        const std::size_t jm = b.at(m, j);
                        for (int c = 0; c < 6; ++c) mv[c] = Mnc * b.x[m][jm].segment(c * n, n);
                        for (int sp = 0; sp < 2; ++sp) mv[6 + sp] = Mnc * b.xs[m][jm].segment(sp * n, n);
                        for (std::size_t i = 0; i < nc; ++i) {
                            const Eigen::VectorXcd& c8 = b.xc[m][b.atC(m, i)];
                            std::complex<double> A[3] = {0.0, 0.0, 0.0};
                            for (int sp = 0; sp < 2; ++sp)
                                for (int a = 0; a < 3; ++a)
                                    A[a] += c8.segment((6 + sp) * n, n).dot(mv[3 * sp + a]) - c8.segment((3 * sp + a) * n, n).dot(mv[6 + sp]);
                            KpPair& pr = prs[i];
                            pr.wte.push_back(1.5 * (std::norm(A[0]) + std::norm(A[1])));
                            pr.wtm.push_back(3 * std::norm(A[2]));
                            pr.ec.push_back(b.C[m][b.atC(m, i)] * Q);
                            pr.ev.push_back(b.E[m][jm] * Q);
                            pr.eps.push_back(pr.ec.back() - pr.ev.back());
                        }
                    }
                    for (std::size_t i = 0; i < nc; ++i) {
                        KpPair& pr = prs[i];
                        if (pr.eps.size() != b.k.size() || (i / 2 < keepE.size() && !keepE[i / 2])) continue;
                        // (A bound electron's with every hole: as the
                        // parabolic bands'.)
                        if (o.continuum == 1) {
                            const State& e = a_states[0][std::min(i / 2, a_states[0].size() - 1)];
                            if (!confined(0, b.C[0][i], e.pos) && !held(0, e)) continue;
                        }
                        double most = 0;
                        for (std::size_t m = 0; m < pr.wte.size(); ++m) most = std::max(most, pr.wte[m] + pr.wtm[m]);
                        if (!(most > 1e-10)) continue;
                        pr.edge = *std::min_element(pr.eps.begin(), pr.eps.end());
                        if (a_spec.excitons && epsr > 0 && pr.wte[0] + pr.wtm[0] > 1e-10) {
                            const std::pair<int, int> key{int(i / 2), int(j / 2)};
                            auto it = x8.find(key);
                            if (it == x8.end()) {
                                const Eigen::VectorXcd& c8 = b.xc[0][b.atC(0, i)];
                                const Eigen::VectorXcd& v6 = b.x[0][j];
                                auto electron = [&](const Point& pt) {
                                    double v = 0;
                                    for (int c = 0; c < 8; ++c) v += std::norm(at(pt, c8, c * n));
                                    return v;
                                };
                                auto hole = [&](const Point& pt) {
                                    double v = 0;
                                    for (int c = 0; c < 6; ++c) v += std::norm(at(pt, v6, c * n));
                                    return v;
                                };
                                // (Its mass along the axis: a heavy hole's of no Z orbital, a light hole's of 2/3.)
                                auto inverse = [&](const Point& pt) {
                                    const Band& bd = a_bands[std::size_t(std::max(0, pt.d))];
                                    double all = 0, z = 0;
                                    for (int c = 0; c < 6; ++c) {
                                        const double v = std::norm(at(pt, v6, c * n));
                                        all += v;
                                        if (c == 2 || c == 5) z += v;
                                    }
                                    const double share = all > 0 ? std::min(1.0, 1.5 * z / all) : 0.0;
                                    return (1 - share) / bd.mhhZ + share / bd.mlhZ;
                                };
                                const State& e = a_states[0][std::min(i / 2, a_states[0].size() - 1)];
                                const double m = kpMass(j), mh = std::isfinite(m) ? m : 1e3;
                                const double mr = 1 / (1 / e.mPar + 1 / mh) * M0;
                                const Exciton xc = exciton(electron, hole, [&](const Point& pt) { return 1 / a_bands[std::size_t(std::max(0, pt.d))].me; },
                                                           inverse, mr, epsr, o.excitonModel != 1);
                                it = x8.emplace(key, xc).first;
                                // (The first electron's with the highest hole's: the e1-hh1 globals.)
                                if (!std::isfinite(a_spec.Eb)) {
                                    a_spec.Eb = xc.Eb;
                                    a_spec.Ex = pr.eps[0] - xc.Eb;
                                    a_spec.radius = xc.radius;
                                    a_spec.gammaX = xc.gamma;
                                }
                            }
                            pr.Eb = it->second.Eb;
                            pr.radius = it->second.radius;
                            pr.F0 = it->second.F0;
                        }
                        pairs.push_back(std::move(pr));
                    }
                }
            }
        } else
        for (std::size_t i = 0; i < a_states[0].size(); ++i) {
            const State& e = a_states[0][i];
            // (A bound electron's with every hole: as the parabolic bands'.)
            if (!keepE[i] || (o.continuum == 1 && !bound(0, e))) continue;
            // <e| on each equation: Σ w ψe N_eq over the points - an overlap
            // with a hole's component its dot product.
            Eigen::VectorXd load = Eigen::VectorXd::Zero(n);
            for (const Point& pt : a_points) {
                const double fe = pt.w * at(pt, e.x);
                if (fe == 0) continue;
                for (int q = 0; q < pt.n; ++q)
                    if (pt.eq[q] >= 0) load(pt.eq[q]) += fe * pt.N[q];
            }
            const Eigen::VectorXcd loadc = load.cast<std::complex<double>>();
            for (std::size_t j = 0; j < a_kps[0].E[0].size(); ++j) {
                if (j < keepV.size() && !keepV[j]) continue;
                bool bound = false;
                Exciton xc;
                for (const KpBands& b : a_kps) {
                    if (j >= b.E[0].size()) continue;
                    KpPair pr;
                    pr.bands = &b;
                    pr.weight = b.weight;
                    pr.Ep = e.Ep * Q;
                    pr.nr = o.index > 0 ? o.index : e.nr;
                    pr.ie = int(i);
                    pr.jv = int(j);
                    double most = 0;
                    for (std::size_t m = 0; m < b.k.size(); ++m) {
                        const Eigen::VectorXcd& x = b.x[m][b.at(m, j)];
                        std::complex<double> ov[6];
                        for (int c = 0; c < 6; ++c) ov[c] = loadc.dot(x.segment(c * n, n));
                        const double wte = 1.5 * (std::norm(ov[0]) + std::norm(ov[1]) + std::norm(ov[3]) + std::norm(ov[4]));
                        const double wtm = 3 * (std::norm(ov[2]) + std::norm(ov[5])) * (e.Ep > 0 ? e.EpZ / e.Ep : 1.0);
                        const double k = b.k[m];
                        const double Ec = e.E * Q + Hbar * Hbar * k * k / (2 * e.mPar * M0), Ev = b.E[m][b.at(m, j)] * Q;
                        pr.eps.push_back(Ec - Ev);
                        pr.wte.push_back(wte);
                        pr.wtm.push_back(wtm);
                        pr.ec.push_back(Ec);
                        pr.ev.push_back(Ev);
                        most = std::max(most, wte + wtm);
                    }
                    if (!(most > 1e-10)) continue;
                    pr.edge = *std::min_element(pr.eps.begin(), pr.eps.end());
                    if (a_spec.excitons && o.excitonModel != 2 && epsr > 0 && pr.wte[0] + pr.wtm[0] > 1e-10) {
                        if (!bound) {
                            bound = true;
                            const Eigen::VectorXcd& x0 = a_kps[0].x[0][j];
                            auto hole = [&](const Point& pt) {
                                double v = 0;
                                for (int c = 0; c < 6; ++c) v += std::norm(at(pt, x0, c * n));
                                return v;
                            };
                            // (Its mass along the axis: a heavy hole's of no Z
                            // orbital, a light hole's of 2/3.)
                            auto inverse = [&](const Point& pt) {
                                const Band& bd = a_bands[std::size_t(std::max(0, pt.d))];
                                double all = 0, z = 0;
                                for (int c = 0; c < 6; ++c) {
                                    const double v = std::norm(at(pt, x0, c * n));
                                    all += v;
                                    if (c == 2 || c == 5) z += v;
                                }
                                const double share = all > 0 ? std::min(1.0, 1.5 * z / all) : 0.0;
                                return (1 - share) / bd.mhhZ + share / bd.mlhZ;
                            };
                            const double m = kpMass(j), mh = std::isfinite(m) ? m : 1e3;
                            const double mr = 1 / (1 / e.mPar + 1 / mh) * M0;
                            xc = exciton([&](const Point& pt) { return std::pow(at(pt, e.x), 2); }, hole,
                                         [&](const Point& pt) { return 1 / a_bands[std::size_t(std::max(0, pt.d))].me; }, inverse, mr, epsr,
                                         o.excitonModel == 0);
                            // (The first electron state kept with the highest
                            // hole kept - a high field's notch states left
                            // out before them -: the e1-hh1 exciton's
                            // globals.)
                            if (!std::isfinite(a_spec.Eb)) {
                                a_spec.Eb = xc.Eb;
                                a_spec.Ex = pr.eps[0] - xc.Eb;
                                a_spec.radius = xc.radius;
                                a_spec.gammaX = xc.gamma;
                            }
                        }
                        pr.Eb = xc.Eb;
                        pr.radius = xc.radius;
                        pr.F0 = xc.F0;
                    }
                    pairs.push_back(std::move(pr));
                }
            }
        }
        // The coupled subbands' excitons of the k·p bands (4x4, 6x6): each
        // electron state with each valence state a pair, in the axial
        // approximation's channels of the excitons' angular momentum about the
        // axis, J (Winkler's). A valence state's spinor at k - its components
        // e+↑ (J_z 3/2), Z↑ (1/2), e-↑ (-1/2), e+↓ (1/2), Z↓ (-1/2), e-↓ (-3/2),
        // e± = (X ± iY)/√2 - at the angle θ e^(-iμθ) times its own at 0: two
        // pairs' attraction in channel J of each component's Coulomb channel
        // |J - μ|, of their spinors at their k - the heavy and light holes'
        // Bloch parts, mixing along k, coupled. The light's excitons those of
        // J 3/2 (TE by e+↑) and 1/2 (TE by e+↓, TM by Z↑), each twice (-J's
        // the same, by time reversal). The spinors: the bands' first
        // direction's at each k (axial: their own; along [100], [110] or
        // averaged, that direction's taken for all, said), turned onto x, in
        // phases making the bands real (e± by -i on spin up, Z by -i on spin
        // down: the k·p's X-Z terms imaginary, spin and orbit real) - each
        // degenerate set made real and lined up with the k before's -, on a
        // basis of each μ's parts (their principal components at some k),
        // cubic between the bands' k points. The pairs' energies their
        // directions' averaged; the lowest 16 pairs by energy, a Kramers pair
        // not parted.
        if (a_spec.excitons && o.excitonModel == 2 && a_valence != 8 && epsr > 0 && !pairs.empty() && !a_kps.front().k.empty()) {
            using cd = std::complex<double>;
            // (A direction's value at k: linear in k² between its points; past
            // the last an energy on along its last interval, a weight held.)
            auto valueAt = [](const std::vector<double>& ks, const std::vector<double>& v, double k, bool extend) {
                const std::size_t m = std::min(ks.size(), v.size());
                if (m < 2) return m == 1 ? v[0] : 0.0;
                std::size_t i = std::size_t(std::upper_bound(ks.begin(), ks.begin() + std::ptrdiff_t(m), k) - ks.begin());
                if (i >= m) {
                    if (!extend) return v[m - 1];
                    i = m - 1;
                }
                i = std::max<std::size_t>(i, 1);
                const double s0 = ks[i - 1] * ks[i - 1], s1 = ks[i] * ks[i];
                return v[i - 1] + (s1 > s0 ? (k * k - s0) / (s1 - s0) : 0.0) * (v[i] - v[i - 1]);
            };
            struct Series {
                std::vector<const KpPair*> dirs;
                double all = 0, edge = 0;
                int ie = -1, jv = -1;
            };
            std::map<std::pair<int, int>, Series> groups;
            for (const KpPair& pr : pairs) {
                Series& g = groups[{pr.ie, pr.jv}];
                g.ie = pr.ie;
                g.jv = pr.jv;
                g.dirs.push_back(&pr);
                g.all += pr.weight;
                g.edge += pr.weight * pr.eps[0];
            }
            std::vector<std::shared_ptr<Series>> chosen;
            for (auto& [key, g] : groups) {
                g.edge /= g.all > 0 ? g.all : 1.0;
                chosen.push_back(std::make_shared<Series>(g));
            }
            std::sort(chosen.begin(), chosen.end(), [](const auto& a, const auto& b) { return a->edge < b->edge; });
            const std::size_t most = o.excitonPairs > 0 ? std::size_t(o.excitonPairs) : 16;
            if (chosen.size() > most) {
                // (A Kramers pair - one edge - kept whole.)
                std::size_t cut = most;
                while (cut > 1 && std::abs(chosen[cut]->edge - chosen[cut - 1]->edge) < 1e-6 * Q) --cut;
                a_s.log << tr("%1: the %2 lowest of the %3 pairs coupled, the others free steps").arg(a_node->name()).arg(cut).arg(chosen.size());
                chosen.resize(cut);
            }
            auto average = [valueAt](const Series& g, double k, int what) {
                double v = 0;
                for (const KpPair* d : g.dirs) {
                    const std::vector<double>& y = what == 0 ? d->eps : what == 1 ? d->wte : d->wtm;
                    v += d->weight * valueAt(d->bands->k, y, k, what == 0);
                }
                return g.all > 0 ? v / g.all : v;
            };
            const KpBands& b0 = a_kps.front();
            const int M = int(b0.k.size());
            const double dk = M > 1 ? b0.k[1] - b0.k[0] : 0.0;
            std::size_t nv = b0.E[0].size();
            for (const std::vector<double>& E : b0.E) nv = std::min(nv, E.size());
            std::vector<int> states, electrons;
            for (const auto& g : chosen) {
                states.push_back(g->jv);
                electrons.push_back(g->ie);
            }
            for (std::vector<int>* v : {&states, &electrons}) {
                std::sort(v->begin(), v->end());
                v->erase(std::unique(v->begin(), v->end()), v->end());
            }
            const auto indexOf = [](const std::vector<int>& v, int x) { return int(std::lower_bound(v.begin(), v.end(), x) - v.begin()); };
            // (The states to the deepest used and, at each k, its degenerate
            // partner: deeper ones the bands' solve may have found one of a
            // Kramers pair of.)
            if (!states.empty() && states.back() + 1 < int(nv)) {
                std::size_t use = std::size_t(states.back()) + 1;
                for (bool more = true; more;) {
                    more = false;
                    for (std::size_t m = 0; m < b0.E.size(); ++m)
                        while (use < nv && std::abs(b0.E[m][b0.at(m, use)] - b0.E[m][b0.at(m, use - 1)]) < 1e-7) {
                            ++use;
                            more = true;
                        }
                }
                nv = use;
            }
            if (nv == 0 || states.back() >= int(nv)) {
                a_s.log << tr("Warning: %1: the coupled excitons want the k·p bands' every state at every k: some k lacks one - left out")
                               .arg(a_node->name());
            } else {
                if (b0.theta != 0 || a_inplane != 0)
                    a_s.log << tr("%1: the coupled excitons' hole spinors the bands' at θ %2° (%3), taken for every direction: the axial "
                                  "approximation")
                                   .arg(a_node->name())
                                   .arg(b0.theta * 180 / M_PI, 0, 'f', 1)
                                   .arg(kpDirection());
                // The mass matrix, by equation; of six components each.
                Eigen::SparseMatrix<double> Mn(n, n);
                {
                    std::vector<Eigen::Triplet<double>> tm;
                    for (const Point& pt : a_points)
                        for (int q = 0; q < pt.n; ++q)
                            for (int r = 0; r < pt.n; ++r)
                                if (pt.eq[q] >= 0 && pt.eq[r] >= 0) tm.emplace_back(pt.eq[q], pt.eq[r], pt.w * pt.N[q] * pt.N[r]);
                    Mn.setFromTriplets(tm.begin(), tm.end());
            }
            const Eigen::SparseMatrix<cd> Mnc = Mn.cast<cd>();
            auto mass6 = [&Mnc, n](const Eigen::VectorXcd& v) {
                Eigen::VectorXcd r(v.size());
                for (int c = 0; c < int(v.size()) / n; ++c) r.segment(c * n, n) = Mnc * v.segment(c * n, n);
                return r;
            };
            // (Circular, in the real phases, turned onto x.)
            const double mu[6] = {1.5, 0.5, -0.5, 0.5, -0.5, -1.5};
            const double theta0 = b0.theta;
            auto circular = [n, theta0, &mu](const Eigen::VectorXcd& x) {
                const cd I(0, 1);
                const double r2 = 1 / std::sqrt(2.0);
                Eigen::VectorXcd y(6 * n);
                for (int sp = 0; sp < 2; ++sp) {
                    const cd te = sp == 0 ? -I : cd(1), tz = sp == 0 ? cd(1) : -I;
                    const auto X = x.segment(3 * sp * n, n), Y = x.segment((3 * sp + 1) * n, n), Z = x.segment((3 * sp + 2) * n, n);
                    y.segment(3 * sp * n, n) = te * r2 * (X - I * Y);
                    y.segment((3 * sp + 1) * n, n) = tz * Z;
                    y.segment((3 * sp + 2) * n, n) = te * r2 * (X + I * Y);
                }
                if (theta0 != 0)
                    for (int c = 0; c < 6; ++c) y.segment(c * n, n) *= std::exp(I * (mu[c] * theta0));
                return y;
            };
            // A degenerate set's real basis: its vectors' real and imaginary
            // parts' span, M-orthonormal - false if wider than the set (the
            // bands not real in these phases).
            double unreal = 0;
            auto realify = [&mass6, &unreal](std::vector<Eigen::VectorXcd>& v) {
                const int g = int(v.size());
                const Eigen::Index len = v.front().size();
                Eigen::MatrixXcd cand(len, 2 * g), mc(len, 2 * g);
                for (int i = 0; i < g; ++i) {
                    cand.col(2 * i) = v[std::size_t(i)].real().cast<cd>();
                    cand.col(2 * i + 1) = v[std::size_t(i)].imag().cast<cd>();
                }
                for (int i = 0; i < 2 * g; ++i) mc.col(i) = mass6(cand.col(i));
                const Eigen::MatrixXd G = (cand.adjoint() * mc).real();
                Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(0.5 * (G + G.transpose()));
                const double top = es.eigenvalues()(2 * g - 1), rest = es.eigenvalues()(g - 1);
                unreal = std::max(unreal, rest / top);
                if (!(rest <= 1e-6 * top)) return false;
                for (int i = 0; i < g; ++i)
                    v[std::size_t(i)] = (cand * es.eigenvectors().col(2 * g - 1 - i)).real().cast<cd>() / std::sqrt(es.eigenvalues()(2 * g - 1 - i));
                return true;
            };
            // The states at the bands' k point m: circular, each degenerate
            // set (one energy to 0.1 µeV) made real (realMode) and lined up
            // with prev's (given) - the unitary nearest their overlaps
            // (Procrustes').
            auto statesAt = [&b0, nv, &circular, &realify, &mass6](int m, bool realMode, const std::vector<Eigen::VectorXcd>* prev, bool* ok) {
                std::vector<Eigen::VectorXcd> y(nv);
                for (std::size_t j = 0; j < nv; ++j) y[j] = circular(b0.x[std::size_t(m)][b0.at(std::size_t(m), j)]);
                for (std::size_t a = 0; a < nv;) {
                    std::size_t e = a + 1;
                    while (e < nv && std::abs(b0.E[std::size_t(m)][b0.at(std::size_t(m), e)] - b0.E[std::size_t(m)][b0.at(std::size_t(m), e - 1)]) < 1e-7) ++e;
                    std::vector<Eigen::VectorXcd> v(y.begin() + std::ptrdiff_t(a), y.begin() + std::ptrdiff_t(e));
                    if (realMode && !realify(v)) *ok = false;
                    const int g = int(e - a);
                    Eigen::MatrixXcd R = Eigen::MatrixXcd::Identity(g, g);
                    if (prev) {
                        Eigen::MatrixXcd O(g, g);
                        for (int r = 0; r < g; ++r) {
                            const Eigen::VectorXcd mp = mass6((*prev)[a + std::size_t(r)]);
                            for (int c = 0; c < g; ++c) O(r, c) = mp.dot(v[std::size_t(c)]);
                        }
                        Eigen::JacobiSVD<Eigen::MatrixXcd> svd(O, Eigen::ComputeFullU | Eigen::ComputeFullV);
                        R = svd.matrixV() * svd.matrixU().adjoint();
                    }
                    for (int c = 0; c < g; ++c) {
                        Eigen::VectorXcd s = Eigen::VectorXcd::Zero(v.front().size());
                        for (int r = 0; r < g; ++r) s += v[std::size_t(r)] * R(r, c);
                        y[a + std::size_t(c)] = realMode ? Eigen::VectorXcd(s.real().cast<cd>()) : s;
                    }
                    a = e;
                }
                return y;
            };
            // Each electron's overlaps with an equation's: Σ w ψe N.
            std::vector<Eigen::VectorXcd> loads;
            for (int ie : electrons) {
                Eigen::VectorXd load = Eigen::VectorXd::Zero(n);
                for (const Point& pt : a_points) {
                    const double fe = pt.w * at(pt, a_states[0][std::size_t(ie)].x);
                    if (fe == 0) continue;
                    for (int q = 0; q < pt.n; ++q)
                        if (pt.eq[q] >= 0) load(pt.eq[q]) += fe * pt.N[q];
                }
                loads.push_back(load.cast<cd>());
            }
            // The basis, each μ's (the components of one J_z); the states'
            // coefficients on it and the electrons' overlaps with each
            // component at each k point - real phases if the bands allow,
            // else complex.
            const std::vector<std::vector<int>> muOf = {{0}, {1, 3}, {2, 4}, {5}};
            std::vector<Eigen::VectorXcd> basis;
            std::vector<std::vector<Eigen::VectorXcd>> coefs;                         // [state][m]
            std::vector<std::vector<std::vector<Eigen::Matrix<cd, 6, 1>>>> overlaps;   // [electron][state][m]
            std::vector<std::vector<std::array<std::vector<double>, 2>>> weights;      // [electron][state]: TE's, TM's [m]
            double captured = 1;   // the least share of a state the basis holds (k to the bands' top)
            bool realMode = true;
            for (int attempt = 0; attempt < 2; ++attempt) {
                realMode = attempt == 0;
                bool ok = true;
                unreal = 0;
                // Its principal components at k points 0, 1, 2, 4, 8, ... and the
                // last: those holding all but 1e-5 of them, 12 at most.
                basis.clear();
                std::vector<std::vector<Eigen::VectorXcd>> samples;
                for (int m = 0;; m = std::max(1, 2 * m)) {
                    const int at = std::min(m, M - 1);
                    samples.push_back(statesAt(at, realMode, nullptr, &ok));
                    if (at == M - 1) break;
                }
                if (!ok) continue;
                for (const std::vector<int>& comps : muOf) {
                    const Eigen::Index len = Eigen::Index(comps.size()) * n;
                    const Eigen::Index cols = Eigen::Index(samples.size() * states.size());
                    Eigen::MatrixXcd S(len, cols), MS(len, cols);
                    Eigen::Index col = 0;
                    for (const std::vector<Eigen::VectorXcd>& y : samples)
                        for (int j : states) {
                            for (std::size_t c = 0; c < comps.size(); ++c) {
                                S.col(col).segment(Eigen::Index(c) * n, n) = y[std::size_t(j)].segment(comps[c] * n, n);
                                MS.col(col).segment(Eigen::Index(c) * n, n) = Mnc * y[std::size_t(j)].segment(comps[c] * n, n);
                            }
                            ++col;
                        }
                    // (Real: a real solve, its vectors real.)
                    const Eigen::MatrixXcd G = S.adjoint() * MS;
                    Eigen::VectorXd lam;
                    Eigen::MatrixXcd vec;
                    if (realMode) {
                        const Eigen::MatrixXd Gr = G.real();
                        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(0.5 * (Gr + Gr.transpose()));
                        lam = es.eigenvalues();
                        vec = es.eigenvectors().cast<cd>();
                    } else {
                        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXcd> es(0.5 * (G + G.adjoint()));
                        lam = es.eigenvalues();
                        vec = es.eigenvectors();
                    }
                    double total = 0;
                    for (Eigen::Index i = 0; i < cols; ++i) total += std::max(0.0, lam(i));
                    double left = total;
                    for (Eigen::Index i = cols - 1; i >= 0 && i >= cols - 12 && left > 1e-5 * total && lam(i) > 1e-12 * total; --i) {
                        const Eigen::VectorXcd b = S * vec.col(i) / std::sqrt(lam(i));
                        Eigen::VectorXcd full = Eigen::VectorXcd::Zero(6 * n);
                        for (std::size_t c = 0; c < comps.size(); ++c) full.segment(comps[c] * n, n) = b.segment(Eigen::Index(c) * n, n);
                        basis.push_back(realMode ? Eigen::VectorXcd(full.real().cast<cd>()) : full);
                        left -= lam(i);
                    }
                }
                std::vector<Eigen::VectorXcd> mb;
                for (const Eigen::VectorXcd& b : basis) mb.push_back(mass6(b));
                coefs.assign(states.size(), std::vector<Eigen::VectorXcd>(std::size_t(M)));
                overlaps.assign(electrons.size(), std::vector<std::vector<Eigen::Matrix<cd, 6, 1>>>(states.size(), std::vector<Eigen::Matrix<cd, 6, 1>>(std::size_t(M))));
                captured = 1;
                std::vector<Eigen::VectorXcd> prev;
                for (int m = 0; m < M && ok; ++m) {
                    std::vector<Eigen::VectorXcd> y = statesAt(m, realMode, m > 0 ? &prev : nullptr, &ok);
                    for (std::size_t s = 0; s < states.size(); ++s) {
                        const Eigen::VectorXcd& x = y[std::size_t(states[s])];
                        Eigen::VectorXcd cf(Eigen::Index(basis.size()));
                        for (std::size_t b = 0; b < basis.size(); ++b) cf(Eigen::Index(b)) = mb[b].dot(x);
                        captured = std::min(captured, cf.squaredNorm());
                        coefs[s][std::size_t(m)] = cf;
                        for (std::size_t e = 0; e < electrons.size(); ++e)
                            for (int c = 0; c < 6; ++c) overlaps[e][s][std::size_t(m)](c) = loads[e].dot(x.segment(c * n, n));
                    }
                    prev = std::move(y);
                    // (Orthonormal on the basis, those it holds most of
                    // first, Gram and Schmidt's: the excitons take their
                    // products as orthonormal. A state it holds little of is
                    // a projection's noise, which overlapped another's by
                    // 0.75 at the grid's top in the check of 21 October -
                    // Miller's well, 14 valence subbands on 48 functions: the
                    // overlaps lost their positiveness, an exciton bound by
                    // 62.6 eV. Those it holds, as they were.)
                    if (states.size() > 1 && !basis.empty()) {
                        std::vector<std::size_t> byHeld(states.size());
                        std::iota(byHeld.begin(), byHeld.end(), 0);
                        std::stable_sort(byHeld.begin(), byHeld.end(), [&coefs, m](std::size_t a, std::size_t b) {
                            return coefs[a][std::size_t(m)].squaredNorm() > coefs[b][std::size_t(m)].squaredNorm();
                        });
                        std::vector<const Eigen::VectorXcd*> done;
                        for (std::size_t s2 : byHeld) {
                            Eigen::VectorXcd& v = coefs[s2][std::size_t(m)];
                            const Eigen::VectorXcd was = v;
                            for (int pass = 0; pass < 2; ++pass)
                                for (const Eigen::VectorXcd* u : done) v -= *u * u->dot(v);
                            const double norm = v.norm();
                            v = norm > 1e-9 * std::max(was.norm(), 1e-300) ? Eigen::VectorXcd(v / norm) : Eigen::VectorXcd(was / std::max(was.norm(), 1e-300));
                            if (realMode) v = v.real().cast<cd>();
                            done.push_back(&v);
                        }
                    }
                }
                if (ok) break;
            }
            a_s.log << tr("%1: the coupled excitons' spinors on %2 functions of their J_z, holding %3 % of each at least%4")
                           .arg(a_node->name())
                           .arg(basis.size())
                           .arg(100 * captured, 0, 'f', 2)
                           .arg(realMode ? QString() : tr("; the bands not real in their phases: complex"));
            // Each pair's own weights at the bands' k points: TE (3/2) Σ |<e|e±>|²,
            // TM 3 Σ |<e|Z>|² by Kane's Ep along the axis - the bands' own.
            weights.assign(electrons.size(), std::vector<std::array<std::vector<double>, 2>>(states.size()));
            for (std::size_t e = 0; e < electrons.size(); ++e) {
                const State& st = a_states[0][std::size_t(electrons[e])];
                const double zr = st.Ep > 0 ? st.EpZ / st.Ep : 1.0;
                for (std::size_t s = 0; s < states.size(); ++s)
                    for (const Eigen::Matrix<cd, 6, 1>& v : overlaps[e][s]) {
                        weights[e][s][0].push_back(1.5 * (std::norm(v(0)) + std::norm(v(2)) + std::norm(v(3)) + std::norm(v(5))));
                        weights[e][s][1].push_back(3 * zr * (std::norm(v(1)) + std::norm(v(4))));
                    }
            }
            exciton::Envelopes env = xEnvelopes();
            for (int ie : electrons) env.electrons.push_back(xEnvelope(a_states[0][std::size_t(ie)].x));
            for (const Eigen::VectorXcd& b : basis) {
                std::vector<std::vector<cd>> spinor(6);
                for (int c = 0; c < 6; ++c)
                    for (std::size_t p : xOrder) spinor[std::size_t(c)].push_back(std::sqrt(a_points[p].w) * at(a_points[p], b, c * n));
                env.holes.push_back(std::move(spinor));
            }
            env.angular.assign(std::begin(mu), std::end(mu));
            xStiffness(env, 0);
            // The e1-hh1's edge: the first electron state's taken with its
            // brightest TE valence state at k = 0, of the lowest.
            double e1hh1 = NaN, e1hh1Weight = 0;
            const int firstE = electrons.empty() ? 0 : electrons.front();
            for (const std::shared_ptr<Series>& g : chosen) {
                const double w0TE = average(*g, 0, 1);
                if (g->ie == firstE && w0TE > 0 && !std::isfinite(e1hh1)) {
                    e1hh1 = g->edge;
                    e1hh1Weight = g->dirs.front()->Ep / g->dirs.front()->nr * w0TE;
                }
                for (const KpPair* d : g->dirs) const_cast<KpPair*>(d)->coupled = true;
            }
            // The channels: J, its TE component and TM (-1: none).
            struct Channel {
                double J;
                int te, tm;
            };
            std::vector<const CoupledBlock*> mine;
            for (const Channel& ch : {Channel{1.5, 0, -1}, Channel{0.5, 3, 1}}) {
                std::vector<exciton::Pair> xp;
                for (const std::shared_ptr<Series>& g : chosen) {
                    const int si = indexOf(states, g->jv), ei = indexOf(electrons, g->ie);
                    const double scale = g->dirs.front()->Ep / g->dirs.front()->nr;
                    exciton::Pair x;
                    x.electron = ei;
                    x.energy = [g, average](double k) { return average(*g, k, 0); };
                    const std::vector<Eigen::VectorXcd>* cs = &coefs[std::size_t(si)];
                    x.holeAt = [cs, dk](double k, std::vector<cd>& c) {
                        const Eigen::VectorXcd v = cubicAt(*cs, dk, k);
                        const double norm = v.norm();
                        for (Eigen::Index i = 0; i < v.size() && i < Eigen::Index(c.size()); ++i) c[std::size_t(i)] = norm > 0 ? v(i) / norm : cd(0);
                    };
                    // (Its amplitudes: the spinor's overlaps, cubic between the
                    // bands' k points, each polarization's |·|² scaled to its own
                    // at the points linear in k² between them - as the bands'
                    // weights: the free continuum the spectrum's without
                    // excitons - and by the directions' average over this
                    // direction's. Its own, not the bands' state's: where states
                    // are one energy the bands' are any mixture of them.)
                    const std::vector<Eigen::Matrix<cd, 6, 1>>* ovs = &overlaps[std::size_t(ei)][std::size_t(si)];
                    const std::array<std::vector<double>, 2>* own = &weights[std::size_t(ei)][std::size_t(si)];
                    const KpPair* d0 = nullptr;
                    for (const KpPair* d : g->dirs)
                        if (d->bands == &b0) d0 = d;
                    const int te = ch.te, tm = ch.tm;
                    x.amplitude = [g, average, valueAt, d0, own, ovs, &b0, dk, scale, te, tm](double k, std::vector<cd>& a) {
                        const Eigen::Matrix<cd, 6, 1> v = cubicAt(*ovs, dk, k);
                        const double sTE = 1.5 * (std::norm(v(0)) + std::norm(v(2)) + std::norm(v(3)) + std::norm(v(5)));
                        const double sTM = 3 * (std::norm(v(1)) + std::norm(v(4)));
                        double w[2] = {valueAt(b0.k, (*own)[0], k, false), valueAt(b0.k, (*own)[1], k, false)};
                        for (int c = 0; c < 2; ++c) {
                            const double here = d0 ? valueAt(b0.k, c == 0 ? d0->wte : d0->wtm, k, false) : 0.0;
                            if (here > 0) w[c] *= average(*g, k, c + 1) / here;
                        }
                        a[0] = sTE > 0 ? std::sqrt(1.5 * scale * std::max(0.0, w[0]) / sTE) * v(te) : cd(0);
                        a[1] = tm >= 0 && sTM > 0 ? std::sqrt(3 * scale * std::max(0.0, w[1]) / sTM) * v(tm) : cd(0);
                    };
                    xp.push_back(std::move(x));
                }
                exciton::Options xo = xOptions(epsr, 2, chosen.front()->edge);
                xo.channel = ch.J;
                CoupledBlock blk;
                blk.r = exciton::solve(env, xp, xo);
                if (blk.r.energy.empty()) {
                    *error = tr("%1: the coupled excitons: %2").arg(a_node->name(), blk.r.note);
                    return -1;
                }
                blk.spin = 2;
                blk.te = {0};
                blk.tm = {1};
                blk.edge = chosen.front()->edge;
                blk.cells();
                if (ch.J == 1.5) xSpacing(blk, int(chosen.size()), xo.maxSize);
                xDrawn(blk);
                xBlocks.push_back(std::move(blk));
            }
            for (std::size_t b = xBlocks.size() - 2; b < xBlocks.size(); ++b) mine.push_back(&xBlocks[b]);
            if (std::isfinite(e1hh1)) xGlobals(mine, e1hh1, e1hh1Weight);
            // (A valence pair in the wells the count left out, near the last
            // taken: it mixes with them along k within the excitons' reach.)
            if (std::isfinite(nextVGap) && nextVGap < 0.03)
                a_s.log << tr("Warning: %1: the coupled excitons take the spectrum's %2 valence subbands; the next in the wells, %3 meV "
                              "below the last taken in them, mixes with them along k within the excitons' reach - their lines want it: \"vstates\" "
                              "%4 or more (a 9.4 nm GaAs well's hh2, 8 meV below its lh1: with it the TM line 28 % stronger, TE 4 %)")
                               .arg(a_node->name())
                               .arg(nextVKept)
                               .arg(formatNumber(nextVGap * 1e3, 3))
                               .arg(nextVCount);
            }
        }
    }
    // The continuum's ripple at this line's width, by what was measured (a
    // 10 nm GaAs well's 4x4 bands: at the ripple's peaks ±11 % at 0.5 meV,
    // ±4 % at 1, ±1.5 % at 2 - each state spread over its pairs' cells, a
    // larger size little help), past 2 meV on that slope: said where some 2 %
    // or more. (Said by the widened spacing against the line's, 152 meV
    // "apart" warned of a spectrum that a size of 4000 moved by 1 % at most:
    // the update check of 21 October, 4.)
    if (widest.spacing > 0 && gamma > 0) {
        const double meV = gamma / Q * 1e3;
        const double at[3] = {0.5, 1, 2}, ripple[3] = {11, 4, 1.5};
        double percent = ripple[0];
        if (meV >= at[2]) percent = ripple[2] * std::pow(at[2] / meV, std::log(ripple[1] / ripple[2]) / std::log(at[2] / at[1]));
        else if (meV > at[0]) {
            const int i = meV < at[1] ? 0 : 1;
            const double u = std::log(meV / at[i]) / std::log(at[i + 1] / at[i]);
            percent = std::exp((1 - u) * std::log(ripple[i]) + u * std::log(ripple[i + 1]));
        }
        if (percent >= 2)
            a_s.log << tr("Warning: %1: the coupled excitons' continuum states %2 meV apart (%3 on %4 k points: its %5 unknowns, "
                          "\"excitonsize\"): above the edges, where pairs' continua mix, its absorption ripples about its trend some "
                          "±%6 % at the ripple's peaks at this line's %7 meV - a wider line smooths it, a larger size little (a 10 nm "
                          "GaAs well's 4x4 bands from 1.48 to 1.50 eV: ±11 % at 0.5 meV, ±4 % at 1, ±1.5 % at 2; its rms under half that)")
                           .arg(a_node->name())
                           .arg(widest.spacing / Q * 1e3, 0, 'g', 3)
                           .arg(widest.pairs == 1 ? tr("1 pair") : tr("%1 pairs").arg(widest.pairs))
                           .arg(widest.points)
                           .arg(widest.size)
                           .arg(qRound(percent))
                           .arg(formatNumber(meV, 3));
    }
    // A k interval's line at E: its energies' spread [ε0, ε1] its density
    // (1/J), broadened by the line's shape.
    auto spread = [&](double e0, double e1, double E) {
        const double lo = std::min(e0, e1), hi = std::max(e0, e1);
        if (hi - lo < 1e-9 * Q) return gamma > 0 ? pdf(0.5 * (lo + hi) - E) : 0.0;
        if (gamma > 0) return (cdf(hi - E) - cdf(lo - E)) / (hi - lo);
        return E >= lo && E < hi ? 1 / (hi - lo) : 0.0;
    };
    std::vector<double> spTE, spTM;
    // What the transitions well above each energy (their least 5 half widths
    // above it) add there: their lines' tails alone - a Lorentzian's reach
    // far, and with them the spectrum depends on how many states it takes.
    const double far = 5 * gamma;
    std::vector<double>& tailTE = a_spec.farTE;
    std::vector<double>& tailTM = a_spec.farTM;
    tailTE.clear();
    tailTM.clear();
    // The energies asked, then those the Kramers-Kronig index wants between
    // them: at most an eighth of the line's half width apart (its absorption
    // there alone, not a point of the spectrum) - the integral of energies
    // as far apart as the line is wide was 60 % off at a 6 meV line every
    // 10 meV, 1e-4 with these (2e-3 at a quarter).
    std::vector<double> sweep = o.energies, kkE, kkTE, kkTM;
    if (o.kramersKronig && gamma > 0 && o.energies.size() >= 2) {
        std::vector<double> sorted = o.energies;
        std::sort(sorted.begin(), sorted.end());
        double fine = gamma / 8, extra = 0;
        for (std::size_t k = 1; k < sorted.size(); ++k) extra += std::max(0.0, std::ceil((sorted[k] - sorted[k - 1]) / fine) - 1);
        if (extra > 20000) fine *= extra / 20000;   // (at most some 20 000 more)
        for (std::size_t k = 1; k < sorted.size(); ++k) {
            const double gap = sorted[k] - sorted[k - 1];
            const int n = int(std::ceil(gap / fine));
            for (int j = 1; j < n; ++j) sweep.push_back(sorted[k - 1] + gap * j / n);
        }
    }
    const std::size_t asked = o.energies.size();
    // (The lines wider with the photon's energy: from the lowest edge, the
    // check of 21 October's wish - Miller's n = 2 and 3 wider than n = 1.)
    double lowestEdge = Inf();
    for (const Transition& t : transitions) lowestEdge = std::min(lowestEdge, t.E);
    for (const KpPair& pr : pairs) lowestEdge = std::min(lowestEdge, pr.edge);
    for (const Bulk& bulk : bulks) lowestEdge = std::min(lowestEdge, bulk.Eg);
    if (!std::isfinite(lowestEdge) && !sweep.empty()) lowestEdge = *std::min_element(sweep.begin(), sweep.end());
    if (o.linewidthSlope > 0 && gamma > 0)
        a_s.log << tr("%1: its lines' width %2 at the lowest edge, %3, and %4 meV more per meV above it")
                       .arg(a_node->name(), electronVolts(gamma), electronVolts(lowestEdge), formatNumber(o.linewidthSlope, 4));
    for (std::size_t point = 0; point < sweep.size(); ++point) {
        const double E = sweep[point];
        if (o.linewidthSlope > 0 && gamma > 0) setWidth(gamma + o.linewidthSlope * std::max(0.0, E - lowestEdge));
        double te = 0, tm = 0, ste = 0, stm = 0, teTail = 0, tmTail = 0;
        for (const KpPair& pr : pairs) {
            if (pr.coupled) continue;   // (its line and continuum the coupled excitons')
            const double te0 = te, tm0 = tm;
            const auto tally = qScopeGuard([&] {
                if (pr.edge > E + far) {
                    teTail += te - te0;
                    tmTail += tm - tm0;
                }
            });
            // α = (π q² ħ / (n c ε0 m0² E)) |Mb|² Σ ∫ (k dk/2π) w (f_v - f_c) L / (the wells' size).
            // (Its direction's share of the plane.)
            const double C = M_PI * Q * Q * Hbar / (pr.nr * C0 * Eps0 * M0 * M0 * E) * (M0 * pr.Ep / 6) / per * pr.weight;
            const std::vector<double>& ks = pr.bands->k;
            for (std::size_t m = 1; m < pr.eps.size(); ++m) {
                const double mu = (ks[m] * ks[m] - ks[m - 1] * ks[m - 1]) / (4 * M_PI);
                // Near the photon, cells of a quarter of kT or the line's
                // width at most (the occupations their own in each; k² and
                // the energies linear in t); unbroadened, where its energy is
                // E - the electron's and hole's energies there, their
                // difference E, so that its emission is its absorption's over
                // e^((E - ΔF)/kT) - 1.
                const double e0 = pr.eps[m - 1], e1 = pr.eps[m], de = std::abs(e1 - e0);
                int cellsHere = 1;
                if (gamma > 0 && std::abs(E - 0.5 * (e0 + e1)) < 20 * gamma + de)
                    cellsHere = std::clamp(int(std::ceil(de / (0.25 * std::min(kT, gamma)))), 1, 32);
                for (int q = 0; q < cellsHere; ++q) {
                    const double t0 = double(q) / cellsHere, t1 = double(q + 1) / cellsHere;
                    double ea = e0 + (e1 - e0) * t0, eb = e0 + (e1 - e0) * t1, part = 1;
                    // (The window's: the part of the cell's energies within it.)
                    if (window && !clip(ea, eb, &part)) continue;
                    const double l = mu * (t1 - t0) * part * spread(ea, eb, E);
                    if (l == 0) continue;
                    double t = 0.5 * (t0 + t1);
                    if (!(gamma > 0) && de > 1e-9 * Q) t = std::clamp((E - e0) / (e1 - e0), 0.0, 1.0);
                    auto mix = [&](const std::vector<double>& v) { return v[m - 1] + t * (v[m] - v[m - 1]); };
                    double S = 1;
                    if (pr.Eb > 0) {
                        const double eps = mix(pr.eps) - pr.eps[0];
                        S = eps > 0 ? 2 / (1 + std::exp(-2 * M_PI * std::sqrt(pr.Eb / 4 / eps))) : 2.0;
                    }
                    const double Ec = mix(pr.ec), Ev = mix(pr.ev), f = fadeAt(mix(pr.eps));
                    if (f == 0) continue;
                    const double occ = (a_spec.carriers ? fermi((Ev - a_spec.Fv) / kT) - fermi((Ec - a_spec.Fc) / kT) : 1.0) * S * f;
                    const double emi = (a_spec.carriers ? fermi((Ec - a_spec.Fc) / kT) * (1 - fermi((Ev - a_spec.Fv) / kT)) : 0.0) * f;
                    const double wte = mix(pr.wte), wtm = mix(pr.wtm);
                    te += C * wte * occ * l;
                    tm += C * wtm * occ * l;
                    ste += C * wte * emi * l;
                    stm += C * wtm * emi * l;
                }
            }
            // Past the grid's top, the line's tail: the last interval's
            // density on, as the parabolic transitions' (1 - cdf(top - δ)).
            const std::size_t last = pr.eps.size() - 1;
            if (gamma > 0 && last >= 1 && pr.eps[last] - pr.eps[last - 1] > 1e-9 * Q && (!window || pr.eps[last] < windowHi)) {
                const double mu = (ks[last] * ks[last] - ks[last - 1] * ks[last - 1]) / (4 * M_PI);
                const double from = window ? std::max(pr.eps[last], windowLo) : pr.eps[last];
                const double l = mu / (pr.eps[last] - pr.eps[last - 1]) * ((window ? cdf(windowHi - E) : 1.0) - cdf(from - E)) * fadeAt(pr.eps[last]);
                const double Ec = pr.ec[last], Ev = pr.ev[last];
                const double occ = a_spec.carriers ? fermi((Ev - a_spec.Fv) / kT) - fermi((Ec - a_spec.Fc) / kT) : 1.0;
                const double emi = a_spec.carriers ? fermi((Ec - a_spec.Fc) / kT) * (1 - fermi((Ev - a_spec.Fv) / kT)) : 0.0;
                te += C * pr.wte[last] * occ * l;
                tm += C * pr.wtm[last] * occ * l;
                ste += C * pr.wte[last] * emi * l;
                stm += C * pr.wtm[last] * emi * l;
            }
            // Its 1s exciton's line: |F(0)|² (2/(πλ²) Bastard's), its weight at
            // k = 0 (the Kramers partner its own: the two spin pairs between
            // them).
            if (pr.Eb > 0 && gamma > 0 && (!window || inWindow(pr.eps[0] - pr.Eb))) {
                const double line = pr.F0 * pdf(E - (pr.eps[0] - pr.Eb));
                te += C * pr.wte[0] * line;
                tm += C * pr.wtm[0] * line;
            }
        }
        for (const Transition& t : transitions) {
            if (t.coupled) continue;   // (its line and continuum the coupled excitons')
            const double te0 = te, tm0 = tm;
            const auto tally = qScopeGuard([&] {
                if (t.E > E + far) {
                    teTail += te - te0;
                    tmTail += tm - tm0;
                }
            });
            const double delta = E - t.E;
            double dos = 0, dosSp = 0;   // 1/(J m²) wells, 1/(J m) wires
            if (wells) {
                double sum = 0, sumSp = 0;
                const bool enhanced = a_spec.excitons && t.Eb > 0;
                // (Fading: the step's cells past the fade's end, each cell's
                // part its transition energy's fadeAt.)
                const double reachT = fading ? std::max(top, barrierGap + o.fade + 20 * gamma - t.E) : top;
                const int cellsT = fading ? std::clamp(int(std::ceil(reachT / step)), 50, 40000) : cells;
                if (gamma > 0 && !a_spec.carriers && !enhanced && !fading && !window) sum = cdf(delta);   // (a step's, broadened: exact)
                else if (gamma > 0) {
                    for (int k = 0; k < cellsT; ++k) {
                        double a = reachT * k / cellsT, b = reachT * (k + 1) / cellsT, part = 1;
                        // (The window's: its energies t.E + ε within it.)
                        if (window) {
                            double ea = t.E + a, eb = t.E + b;
                            if (!clip(ea, eb, &part)) continue;
                            a = ea - t.E;
                            b = eb - t.E;
                        }
                        const double l = line(a, b, delta), f = fadeAt(t.E + (a + b) / 2);
                        if (f == 0) continue;
                        sum += occupation(t, (a + b) / 2) * sommerfeld(t, (a + b) / 2) * l * f;
                        sumSp += emission(t, (a + b) / 2) * l * f;
                    }
                    const double f = fadeAt(t.E + reachT);
                    // (Past the cells: to the window's top where it is one.)
                    const double beyond = window ? (t.E + reachT < windowHi ? line(std::max(reachT, windowLo - t.E), windowHi - t.E, delta) : 0.0)
                                                 : 1 - cdf(reachT - delta);
                    sum += occupation(t, reachT) * sommerfeld(t, reachT) * beyond * f;
                    sumSp += emission(t, reachT) * beyond * f;
                } else if (delta >= 0) {
                    sum = occupation(t, delta) * fadeAt(E);
                    sumSp = emission(t, delta) * fadeAt(E);
                }
                dos = t.mr / (M_PI * Hbar * Hbar) * sum;
                dosSp = t.mr / (M_PI * Hbar * Hbar) * sumSp;
                // Its 1s exciton's line: |F(0)|² (2/(πλ²) Bastard's) per area, for each
                // of the two spin pairs the continuum's mr/(πħ²) counts too
                // (2D: the bound states' |φn(0)|² dn/dE at the edge the
                // continuum's S(0) mr/(2πħ²) a pair).
                if (enhanced && (!window || inWindow(t.E - t.Eb))) dos += 2 * t.F0 * pdf(delta + t.Eb);
            } else {
                // In u = √ε: each cell's mean of ε^(-1/2) is 2/(u_k + u_k+1).
                const double utop = std::sqrt(top);
                double sum = 0, sumSp = 0;
                for (int k = 0; k < cells; ++k) {
                    const double u0 = utop * k / cells, u1 = utop * (k + 1) / cells;
                    double ea = t.E + u0 * u0, eb = t.E + u1 * u1, part = 1;
                    if (window && !clip(ea, eb, &part)) continue;
                    const double l = line(ea - t.E, eb - t.E, delta);
                    sum += occupation(t, (u0 * u0 + u1 * u1) / 2) * 2 / (u0 + u1) * l;
                    sumSp += emission(t, (u0 * u0 + u1 * u1) / 2) * 2 / (u0 + u1) * l;
                }
                const double beyond = window ? (t.E + top < windowHi ? line(std::max(top, windowLo - t.E), windowHi - t.E, delta) : 0.0) : 1 - cdf(top - delta);
                sum += occupation(t, top) / utop * beyond;
                sumSp += emission(t, top) / utop * beyond;
                dos = std::sqrt(2 * t.mr) / (M_PI * Hbar) * sum;
                dosSp = std::sqrt(2 * t.mr) / (M_PI * Hbar) * sumSp;
            }
            // α = (π q² ħ / (n c ε0 m0² E)) |ê·p|² |<e|h>|² ρ / (the wells' size); |Mb|² = m0 Ep / 6.
            const double C = M_PI * Q * Q * Hbar / (t.nr * C0 * Eps0 * M0 * M0 * E) * (M0 * t.Ep / 6) * t.overlap / per;
            te += C * t.te * dos;
            tm += C * t.tm * dos;
            ste += C * t.te * dosSp;
            stm += C * t.tm * dosSp;
        }
        // The coupled subbands' excitons: each state's line - those of the
        // grid's fine part, below its cut -, and past it each pair's free
        // continuum, its line's tail: α = (π q² ħ/(c ε0 m0² E)) (m0/6)
        // Σ |P|² L / (the wells' size), the amplitudes' Kane energy and index
        // in P.
        for (const CoupledBlock& blk : xBlocks) {
            const exciton::Result& r = blk.r;
            const double Cx = M_PI * Q * Q * Hbar / (C0 * Eps0 * M0 * M0 * E) * (M0 / 6) / per * blk.spin;
            auto add = [&](double from, double sTE, double sTM, double l) {
                te += Cx * sTE * l;
                tm += Cx * sTM * l;
                if (from > E + far) {
                    teTail += Cx * sTE * l;
                    tmTail += Cx * sTM * l;
                }
            };
            for (std::size_t n = 0; n < blk.spread.size(); ++n) {
                const double En = r.energy[n];
                if (window && !inWindow(En)) continue;
                double sTE = 0, sTM = 0;
                for (int c : blk.te) sTE += r.strength[std::size_t(c)][n];
                for (int c : blk.tm) sTM += r.strength[std::size_t(c)][n];
                double l = blk.spread[n].empty() ? pdf(E - En) : 0.0;
                for (const exciton::Result::Cell& cell : blk.spread[n])
                    l += cell.share * (cell.hi > cell.lo ? (cdf(cell.hi - E) - cdf(cell.lo - E)) / (cell.hi - cell.lo) : pdf(E - En));
                add(En, sTE, sTM, l * fadeAt(En));
            }
            if (window && !(r.cut < windowHi)) continue;
            const double from = window ? std::max(r.cut, windowLo) : r.cut;
            const double l = ((window ? cdf(windowHi - E) : 1.0) - cdf(from - E)) * fadeAt(r.cut);
            for (const std::vector<double>& b : r.beyond) {
                double bTE = 0, bTM = 0;
                for (int c : blk.te) bTE += b[std::size_t(c)];
                for (int c : blk.tm) bTM += b[std::size_t(c)];
                add(r.cut, bTE, bTM, l);
            }
        }
        for (const Bulk& bulk : bulks) {
            // ∫ f(ε) L(δ - ε) dε in cells (the wells' own as their steps
            // fade, 1 - fadeAt); (1/2π²)(2 m_r/ħ²)^(3/2) its density. f the
            // free carriers' √ε, or Elliott's - 2π√R/(1 - e^(-2π√(R/ε))) above
            // the gap, √ε far above it, and below it its excitons' lines,
            // 4π R^(3/2)/n³ at -R/n² (past n 30 their series' density 2π√R up
            // to the gap) -, R each hole's Rydberg in the bulk's εr. L the
            // spectrum's line, or Urbach's e^(-|x|/E_U)/2E_U (the check of 21
            // October: Miller's barriers' own edge at 1.74-1.76 eV).
            const double delta = E - bulk.Eg, reach = std::max(top, delta + top);
            const int n = std::clamp(int(std::ceil(reach / step)), 50, 40000);
            auto in = [&](double eps) { return bulk.fadesIn ? 1 - fadeAt(bulk.Eg + eps) : 1.0; };
            // (An excess bulk's free carriers, taken off, broadened as the
            // box's states that are theirs: by the spectrum's line.)
            double EU = o.urbach;
            auto cdfL = [&](double x) {
                if (!(EU > 0)) return cdf(x);
                return x < 0 ? 0.5 * std::exp(x / EU) : 1 - 0.5 * std::exp(-x / EU);
            };
            auto pdfL = [&](double x) { return EU > 0 ? std::exp(-std::abs(x) / EU) / (2 * EU) : pdf(x); };
            auto lineL = [&](double a, double b) { return cdfL(b - delta) - cdfL(a - delta); };
            // (An excess bulk's two parts: Elliott's and the free carriers' it
            // takes off.)
            double rho = 0, taken = 0;
            for (double mr : {bulk.mrHH, bulk.mrLH}) {
                for (int pass = 0; pass < (bulk.excess ? 2 : 1); ++pass) {
                    // (An excess bulk: its free carriers' taken off - pass 1.)
                    EU = pass == 0 ? o.urbach : 0.0;
                    const bool broadened = gamma > 0 || EU > 0;
                    const double R = pass == 0 && o.bulkExcitons ? mr * std::pow(Q * Q / (4 * M_PI * Eps0 * bulk.epsr), 2) / (2 * Hbar * Hbar) : 0.0;
                    const double sign = pass == 0 ? 1.0 : -1.0;
                    auto f = [R](double eps) {
                        if (!(R > 0)) return std::sqrt(std::max(0.0, eps));
                        if (!(eps > 0)) return 2 * M_PI * std::sqrt(R);
                        const double x = 2 * M_PI * std::sqrt(R / eps);
                        return x > 700 ? 2 * M_PI * std::sqrt(R) : 2 * M_PI * std::sqrt(R) / (1 - std::exp(-x));
                    };
                    double sum = 0;
                    if (broadened) {
                        for (int k = 0; k < n; ++k) {
                            double a = reach * k / n, b = reach * (k + 1) / n, part = 1;
                            if (window) {
                                double ea = bulk.Eg + a, eb = bulk.Eg + b;
                                if (!clip(ea, eb, &part)) continue;
                                a = ea - bulk.Eg;
                                b = eb - bulk.Eg;
                            }
                            sum += f((a + b) / 2) * lineL(a, b) * in((a + b) / 2);
                        }
                        const double beyond = window ? (bulk.Eg + reach < windowHi ? cdfL(windowHi - bulk.Eg - delta) - cdfL(std::max(reach, windowLo - bulk.Eg) - delta) : 0.0)
                                                     : 1 - cdfL(reach - delta);
                        sum += f(reach) * beyond * in(reach);
                        if (R > 0) {
                            // (Its excitons' lines, and their series' rest up to the gap.)
                            const int lines = 30;
                            for (int m = 1; m <= lines; ++m) {
                                const double at = -R / (m * m);
                                if (window && !inWindow(bulk.Eg + at)) continue;
                                sum += 4 * M_PI * std::pow(R, 1.5) / (m * m * m) * pdfL(delta - at) * in(at);
                            }
                            const double from = -R / (lines * lines);
                            if (!window || inWindow(bulk.Eg)) sum += 2 * M_PI * std::sqrt(R) * lineL(from, 0) * in(0);
                        }
                    } else if (delta > 0)
                        sum = f(delta) * in(delta);
                    (sign > 0 ? rho : taken) += std::pow(2 * mr / (Hbar * Hbar), 1.5) / (2 * M_PI * M_PI) * sum;
                }
            }
            // An excess only adds: where the free part, broadened by the
            // spectrum's line, is more than Elliott's with Urbach's tail -
            // below the gap under a wide line, a Lorentzian's tail far below
            // it -, nothing. (It took more than the box gave: -468 /cm with
            // no carriers, 115 /cm off half an eV below the barriers' gap -
            // the hunt of 22 October, A2.)
            rho = std::max(0.0, rho - taken);
            const double C = M_PI * Q * Q * Hbar / (bulk.nr * C0 * Eps0 * M0 * M0 * E) * (M0 * bulk.Ep / 6) * rho * bulk.weight;
            te += C;
            tm += C;
        }
        if (point >= asked) {
            kkE.push_back(E);
            kkTE.push_back(te);
            kkTM.push_back(tm);
            continue;
        }
        a_spec.E.push_back(E);
        a_spec.te.push_back(te);
        a_spec.tm.push_back(tm);
        spTE.push_back(ste);
        spTM.push_back(stm);
        tailTE.push_back(teTail);
        tailTM.push_back(tmTail);
    }
    setWidth(gamma);
    // With carriers and a Lorentzian line: how much the transitions well
    // above the gain's peak take off it, by their tails alone - so that the
    // spectrum depends on how many states the step takes (a GaAs laser
    // well's TE peak 394 /cm with 2 states, 304 with 4). Said when more than
    // 10 %.
    if (a_spec.carriers && o.lineshape == 0 && !a_spec.E.empty()) {
        double worst = 0, at = NaN;
        QString which;
        for (int pol = 0; pol < 2; ++pol) {
            const std::vector<double>& a = pol == 0 ? a_spec.te : a_spec.tm;
            const std::vector<double>& tail = pol == 0 ? tailTE : tailTM;
            std::size_t peak = 0;
            for (std::size_t k = 1; k < a.size(); ++k)
                if (a[k] < a[peak]) peak = k;
            if (!(a[peak] < 0)) continue;   // (no gain)
            // (Of the gain without them.)
            const double share = std::abs(tail[peak]) / (std::abs(tail[peak]) - a[peak]);
            if (share > worst) {
                worst = share;
                at = a_spec.E[peak];
                which = pol == 0 ? QStringLiteral("TE") : QStringLiteral("TM");
            }
        }
        if (worst > 0.1)
            a_s.log << tr("Warning: %1: the transitions more than 5 half widths above the %2 gain's peak (%3) take %4 % off it by their "
                          "Lorentzian tails alone (%5.alpha%2far): the spectrum depends on how many states the step takes. A Gaussian or "
                          "a sech line's tails hardly reach; or take the states that matter (the Lumerical MQW solver keeps 2 conduction and 4 "
                          "valence subbands)")
                               .arg(a_node->name(), which, electronVolts(at))
                               .arg(qRound(100 * worst))
                               .arg(a_node->tag);
    }
    // With carriers, the spontaneous emission per volume of the wells, per
    // energy and time: (n² E²/(π² ħ³ c²)) of the emission's α averaged over
    // the directions, (2 TE + TM)/3.
    if (a_spec.carriers) {
        a_spec.gspTE = spTE;
        a_spec.gspTM = spTM;
        double total = 0;
        for (std::size_t k = 0; k < a_spec.E.size(); ++k) {
            const double E = a_spec.E[k], n = a_spec.nr;
            a_spec.rsp.push_back(n * n * E * E / (M_PI * M_PI * Hbar * Hbar * Hbar * C0 * C0) * (2 * spTE[k] + spTM[k]) / 3);
            if (k > 0) total += 0.5 * (a_spec.rsp[k] + a_spec.rsp[k - 1]) * std::abs(E - a_spec.E[k - 1]);
        }
        a_spec.Rsp = total;
    }
    // Kramers and Kronig: the index the window's absorption makes, (c ħ/π)
    // P∫ α(E')/(E'² - E²) dE' over its energies (the principal value with
    // α(E) taken out: its own integral analytic). Between two fields only
    // what changes counts - electrorefraction's Δn -, so long as the
    // absorption that changes is inside the window: said when its ends
    // still absorb.
    if (a_spec.E.size() >= 3) {
        // (Over the energies asked and those between for it.)
        std::vector<double> allE = a_spec.E, allTE = a_spec.te, allTM = a_spec.tm;
        allE.insert(allE.end(), kkE.begin(), kkE.end());
        allTE.insert(allTE.end(), kkTE.begin(), kkTE.end());
        allTM.insert(allTM.end(), kkTM.begin(), kkTM.end());
        std::vector<std::size_t> order(allE.size());
        for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
        std::sort(order.begin(), order.end(), [&allE](std::size_t a, std::size_t b) { return allE[a] < allE[b]; });
        std::vector<double> x, aTE, aTM;
        for (std::size_t k : order) {
            x.push_back(allE[k]);
            aTE.push_back(allTE[k]);
            aTM.push_back(allTM[k]);
        }
        const double lo = x.front(), hi = x.back();
        auto kk = [&x](const std::vector<double>& alpha, double E, double aE) { return kramersKronig(x, alpha, E, aE); };
        a_spec.dnTE.assign(a_spec.E.size(), 0.0);
        a_spec.dnTM.assign(a_spec.E.size(), 0.0);
        for (std::size_t k = 0; k < a_spec.E.size(); ++k) {
            a_spec.dnTE[k] = kk(aTE, a_spec.E[k], a_spec.te[k]);
            a_spec.dnTM[k] = kk(aTM, a_spec.E[k], a_spec.tm[k]);
        }
        // What the absorption above the window would add were it its top's
        // all the way: (c ħ/π) α_top ln((top + E)/(top - E))/2E - between two
        // fields their difference what Δn leaves out.
        a_spec.dnTEtail.assign(a_spec.E.size(), 0.0);
        a_spec.dnTMtail.assign(a_spec.E.size(), 0.0);
        for (std::size_t k = 0; k < a_spec.E.size(); ++k) {
            const double E = a_spec.E[k];
            if (!(E > 0) || !(hi - E > 1e-12 * Q)) continue;
            const double w = C0 * Hbar / M_PI * std::log((hi + E) / (hi - E)) / (2 * E);
            a_spec.dnTEtail[k] = w * aTE.back();
            a_spec.dnTMtail[k] = w * aTM.back();
        }
        double most = 0;
        for (std::size_t k = 0; k < x.size(); ++k) most = std::max({most, std::abs(aTE[k]), std::abs(aTM[k])});
        const double atTop = std::max(std::abs(aTE.back()), std::abs(aTM.back()));
        // (The lowest energy asked: those between are above it.)
        const std::size_t first = std::size_t(std::min_element(a_spec.E.begin(), a_spec.E.end()) - a_spec.E.begin());
        const double tail = std::max(std::abs(a_spec.dnTEtail[first]), std::abs(a_spec.dnTMtail[first]));
        if (most > 0 && atTop > 0.05 * most) {
            if (fading && hi >= barrierGap + o.fade)
                // (Above the barriers' gap and the fade the bulk's alone: the
                // same in any field.)
                a_s.log << tr("%1: the Kramers-Kronig index (%5.dnTE, %5.dnTM) of the absorption between %2 and %3: its top the bulk's "
                              "alone (above the barriers' gap and the fade), the same in any field - Δn between fields leaves none of "
                              "it out; the index itself what lies above would raise by some %4 at %2 (%5.dnTEtail)")
                               .arg(a_node->name(), electronVolts(lo), electronVolts(hi), formatNumber(tail, 3), a_node->tag);
            else
                a_s.log << tr("Warning: %1: the Kramers-Kronig index (%7.dnTE, %7.dnTM) of the absorption between %2 and %3: the window's "
                              "top still absorbs %4 % of its most - what changes above it is left out (were it its top's all the way, "
                              "some %5 at %2: %7.dnTEtail, %7.dnTMtail; between fields their difference what Δn misses), and near "
                              "the window's ends the index is uncertain: widen the energies%6")
                               .arg(a_node->name(), electronVolts(lo), electronVolts(hi))
                               .arg(qRound(100 * atTop / most))
                               .arg(formatNumber(tail, 3))
                               .arg(fading ? tr(" past the barriers' gap and the fade, %1").arg(electronVolts(barrierGap + o.fade)) : QString())
                               .arg(a_node->tag);
        }
    }
    // (The k·p bands' transitions their pairs', one direction's; the coupled
    // excitons' among them.)
    std::size_t count = transitions.size(), joined = 0;
    for (const Transition& t : transitions) joined += t.coupled ? 1 : 0;
    if (kp) {
        count = 0;
        for (const KpPair& pr : pairs)
            if (pr.bands == &a_kps.front()) {
                ++count;
                joined += pr.coupled ? 1 : 0;
            }
    }
    if (droppedDirections > 0)
        a_s.log << tr("%1: the coupled excitons' pairs drawn together: their functions drawn together left out at %2 k points, their overlaps "
                      "nought or below there - their hole spinors held too little by their basis -, the excitons there as without them")
                       .arg(a_node->name())
                       .arg(droppedDirections);
    // (None: said as a warning - the 8x8's spurious electron of the check of
    // 21 October left every pair out, the answer silent.)
    if (count == 0)
        a_s.log << tr("Warning: %1: a spectrum of no transitions between the subbands - its absorption %2: see which subbands were left "
                      "out (the log's \"the spectrum's subbands\"), or ask for more (cstates, vstates)")
                       .arg(a_node->name(), fading ? tr("the barriers' bulk absorption above their gap alone") : tr("none"));
    a_s.log << tr("%1: a spectrum of %2 transitions%3 at %4 photon energies, %5%6")
                   .arg(a_node->name())
                   .arg(count)
                   .arg(joined > 0 ? tr(" (%1 of them the coupled excitons')").arg(joined) : QString())
                   .arg(o.energies.size())
                   .arg(a_spec.carriers ? tr("the quasi-Fermi levels %1 apart").arg(electronVolts(a_spec.Fc - a_spec.Fv))
                                        : tr("no carriers"))
                   .arg(a_spec.excitons && std::isfinite(a_spec.Eb)
                            ? tr("; the e1-hh1 exciton bound by %1, %2 across").arg(electronVolts(a_spec.Eb),
                                                                                  formatQuantity(a_spec.radius, Dim::length()))
                            : QString());
    selectMode(0);
    return int(a_spec.E.size());
}

void QuantumProblem::selectSpectrum(int k)
{
    a_specSelected = k;
    a_bandsSelected = -1;
}

int QuantumProblem::solveBands(const SpectrumOptions& o, QString* error)
{
    ++a_inner;
    const auto inner = qScopeGuard([this] { --a_inner; });
    a_rho.clear();
    if (solveModes(o.solve, std::max(1, o.states), 0, error) < 0) return -1;
    if (!a_valence) {
        *error = tr("%1: a Valence Bands step wants the k·p valence bands (its physics' Valence bands: 4x4, 6x6 or 8x8)").arg(a_node->name());
        return -1;
    }
    // Every bound state too: at k = 0, more asked for till the lowest found
    // is not bound (above the valence band edges where the physics ends, a
    // state's own "bound"); then as many pairs as are, where more than
    // asked.
    int count = std::max(1, o.states);
    if (o.bound) {
        const double edge = std::max(a_barrier[1], a_barrier[2]);
        constexpr int most = 120;
        int bound = 0;
        for (int asked = count;; asked = std::min(most, 2 * asked)) {
            if (!kpBands(asked, 0, 1, error, 0)) return -1;
            const std::vector<double>& E = a_kps.front().E.front();
            bound = int(std::count_if(E.begin(), E.end(), [edge](double e) { return e > edge; }));
            if (bound < int(E.size()) || int(E.size()) < 2 * asked || asked >= most) {
                if (bound == int(E.size()) && asked >= most)
                    a_s.log << tr("Warning: %1: more than %2 states bound in the wells: the highest %2 taken").arg(a_node->name()).arg(most);
                break;
            }
        }
        const int pairs = (bound + 1) / 2;
        a_s.log << tr("%1: %2 valence states bound in the wells at k = 0 (above %3 eV, the valence band edge where the stack ends; Kramers' "
                      "pairs both)")
                       .arg(a_node->name())
                       .arg(bound)
                       .arg(formatNumber(edge, 6));
        count = std::max(count, pairs);
    }
    // (The 8x8's conduction states too: as many Kramers pairs as asked.)
    if (!kpBands(count, o.kmax, std::max(2, o.kpoints), error, a_valence == 8 ? std::max(1, o.states) : 0)) return -1;
    a_specSelected = -1;
    const KpBands& b0 = a_kps.front();
    QStringList top;
    for (std::size_t j = 0; j < std::min<std::size_t>(b0.E[0].size(), 8); j += 2) top << formatNumber(b0.E[0][j], 6);
    a_s.log << tr("%1: the %2x%2 k·p valence bands (%3) at %4 values of k to %5; at k = 0 the highest %6 eV%7")
                   .arg(a_node->name())
                   .arg(a_valence)
                   .arg(kpDirection())
                   .arg(b0.k.size())
                   .arg(formatQuantity(b0.k.back(), Dim::of(-1)), top.join(QStringLiteral(", ")), kpSpuriousSaid());
    selectMode(0);
    return int(b0.k.size());
}

void QuantumProblem::selectBands(int k)
{
    a_bandsSelected = k;
}

void QuantumProblem::setGlobals()
{
    const QString p = field().tag + QLatin1Char('.');
    const Dim energy = Dim::of(2, 1, -2);
    const QString carrier[Carriers] = {tr("electron"), tr("heavy hole"), tr("light hole")};
    for (int c = 0; c < Carriers; ++c) {
        if (!a_on[c]) continue;
        const QString n = QLatin1String(CarrierName[c]);
        for (std::size_t k = 0; k < a_states[c].size(); ++k)
            a_s.setGlobal(p + QStringLiteral("E%1%2").arg(n).arg(k + 1), a_states[c][k].E * Q, energy,
                          tr("the %1 state %2's energy").arg(carrier[c]).arg(k + 1), QStringLiteral("eV"));
        const bool has = a_selected >= 0 && a_selected < int(a_states[c].size());
        const State* s = has ? &a_states[c][std::size_t(a_selected)] : nullptr;
        a_s.setGlobal(p + QStringLiteral("E_") + n, s ? s->E * Q : NaN, energy, tr("the %1 state's energy (the one selected)").arg(carrier[c]), QStringLiteral("eV"));
        a_s.setGlobal(p + QStringLiteral("pos_") + n, s ? s->pos : NaN, Dim::length(),
                      tr("where the %1 state is along the growth axis: its mean").arg(carrier[c]));
        a_s.setGlobal(p + QStringLiteral("bound_") + n, s ? (s->bound ? 1.0 : 0.0) : NaN, Dim(),
                      tr("1 when the %1 state is bound (inside the barriers' band edge)").arg(carrier[c]));
    }
    a_s.setGlobal(p + QStringLiteral("state"), a_selected + 1, Dim(), tr("the state's number, from the band edge"));
    // The valley set of each electron state; the carriers per area in each
    // state, in all and in each valley set (a well's, filled to the Fermi
    // levels given).
    if (a_sets > 0)
        for (std::size_t k = 0; k < a_states[0].size(); ++k)
            a_s.setGlobal(p + QStringLiteral("Ve%1").arg(k + 1), a_states[0][k].valley + 1, Dim(),
                          tr("the electron state %1's valley set (from 1, as given)").arg(k + 1));
    const Dim sheet = Dim::of(-2);
    for (int c = 0; c < Carriers; ++c) {
        if (a_sheet[c].empty()) continue;
        double all = 0;
        for (std::size_t k = 0; k < a_sheet[c].size(); ++k) {
            a_s.setGlobal(p + QStringLiteral("N%1%2").arg(QLatin1String(CarrierName[c])).arg(k + 1), a_sheet[c][k], sheet,
                          tr("the %1 state %2's carriers per area").arg(carrier[c]).arg(k + 1), QStringLiteral("1/cm^2"));
            all += a_sheet[c][k];
        }
        if (c == 0) a_s.setGlobal(p + QStringLiteral("Ns"), all, sheet, tr("the electrons per area in the states found"), QStringLiteral("1/cm^2"));
    }
    if (!a_sheet[1].empty() || !a_sheet[2].empty()) {
        double all = 0;
        for (int c = 1; c < Carriers; ++c)
            for (double v : a_sheet[c]) all += v;
        a_s.setGlobal(p + QStringLiteral("Ps"), all, sheet, tr("the holes per area in the states found"), QStringLiteral("1/cm^2"));
    }
    for (std::size_t v = 0; v < a_sheetValley.size(); ++v)
        a_s.setGlobal(p + QStringLiteral("Nv%1").arg(v + 1), a_sheetValley[v], sheet, tr("the electrons per area in valley set %1").arg(v + 1),
                      QStringLiteral("1/cm^2"));
    // The gap the wells make, and the transitions with their overlaps.
    if (a_on[0] && !a_states[0].empty() && ((a_on[1] && !a_states[1].empty()) || (a_on[2] && !a_states[2].empty()))) {
        double top = -Inf();
        for (int c = 1; c < Carriers; ++c)
            if (a_on[c] && !a_states[c].empty()) top = std::max(top, a_states[c].front().E);
        const double gap = (a_states[0].front().E - top) * Q;
        a_s.setGlobal(p + QStringLiteral("Eg"), gap, energy, tr("the effective gap: the first electron state to the highest hole state"), QStringLiteral("eV"));
        a_s.setGlobal(p + QStringLiteral("lambda"), gap > 0 ? 2 * M_PI * Hbar * C0 / gap : NaN, Dim::length(),
                      tr("the wavelength of the effective gap: hc/Eg"));
        if (a_on[1]) {
            a_s.setGlobal(p + QStringLiteral("Edir"), a_directE * Q, energy,
                          tr("the direct transition: the lowest electron to heavy hole one of at least half the largest overlap (a well's "
                             "own e1-hh1, where a field puts the stack's lowest and highest states in different wells)"), QStringLiteral("eV"));
            a_s.setGlobal(p + QStringLiteral("Odir"), a_directO, Dim(), tr("the direct transition's overlap"));
        }
        const std::size_t most = 4;
        for (std::size_t i = 0; i < std::min(most, a_states[0].size()); ++i)
            for (int c = 1; c < Carriers; ++c) {
                if (!a_on[c]) continue;
                for (std::size_t j = 0; j < std::min(most, a_states[c].size()); ++j) {
                    const QString name = QStringLiteral("e%1%2%3").arg(i + 1).arg(QLatin1String(CarrierName[c])).arg(j + 1);
                    a_s.setGlobal(p + QStringLiteral("E") + name, (a_states[0][i].E - a_states[c][j].E) * Q, energy,
                                  tr("the transition's energy, e%1 to %2%3").arg(i + 1).arg(QLatin1String(CarrierName[c])).arg(j + 1), QStringLiteral("eV"));
                    const bool known = i < a_overlap[c].size() && j < a_overlap[c][i].size();
                    a_s.setGlobal(p + QStringLiteral("O") + name, known ? a_overlap[c][i][j] : NaN, Dim(),
                                  tr("the overlap |<e%1|%2%3>|²").arg(i + 1).arg(QLatin1String(CarrierName[c])).arg(j + 1));
                }
            }
    }
    // A Valence Bands step's point: k, each state's energy (Kramers' pairs
    // both, from the highest).
    // (Averaged over the directions: each state's energy their mean.)
    if (!a_kps.empty() && a_bandsSelected >= 0 && a_bandsSelected < int(a_kps[0].k.size())) {
        const std::size_t k = std::size_t(a_bandsSelected);
        a_s.setGlobal(p + QStringLiteral("k"), a_kps[0].k[k], Dim::of(-1), tr("the wave vector in the wells' plane (%1)").arg(kpDirection()),
                      QStringLiteral("1/nm"));
        for (std::size_t j = 0; j < a_kps[0].E[k].size(); ++j) {
            double E = 0;
            for (const KpBands& b : a_kps) E += b.weight * (j < b.E[k].size() ? b.E[k][j] : NaN);
            a_s.setGlobal(p + QStringLiteral("Ev%1").arg(j + 1), E * Q, energy,
                          tr("the valence state %1's energy (k·p; from the highest, Kramers' pairs both)").arg(j + 1), QStringLiteral("eV"));
        }
        for (std::size_t i = 0; i < (a_kps[0].C.empty() ? 0 : a_kps[0].C[k].size()); ++i) {
            double E = 0;
            for (const KpBands& b : a_kps) E += b.weight * (i < b.C[k].size() ? b.C[k][i] : NaN);
            a_s.setGlobal(p + QStringLiteral("Ec%1").arg(i + 1), E * Q, energy,
                          tr("the conduction state %1's energy (the 8x8's; from the lowest, Kramers' pairs both)").arg(i + 1), QStringLiteral("eV"));
        }
    }
    if (a_specSelected >= 0 && a_specSelected < int(a_spec.E.size())) {
        const std::size_t k = std::size_t(a_specSelected);
        const Dim perMetre = Dim::of(-1);
        a_s.setGlobal(p + QStringLiteral("alphaTE"), a_spec.te[k], perMetre,
                      tr("absorption coefficient of the wells, TE (in their plane) - per their thickness, or what the step's Absorption per "
                         "says (qw.Lnorm)"));
        a_s.setGlobal(p + QStringLiteral("alphaTM"), a_spec.tm[k], perMetre, tr("absorption coefficient of the wells, TM (along the growth axis)"));
        a_s.setGlobal(p + QStringLiteral("gTE"), -a_spec.te[k], perMetre, tr("material gain of the wells, TE"));
        a_s.setGlobal(p + QStringLiteral("gTM"), -a_spec.tm[k], perMetre, tr("material gain of the wells, TM"));
        a_s.setGlobal(p + QStringLiteral("Ephoton"), a_spec.E[k], energy, tr("the photon's energy"), QStringLiteral("eV"));
        a_s.setGlobal(p + QStringLiteral("Lw"), a_spec.size, a_1d ? Dim::length() : Dim::of(2),
                      a_1d ? tr("the wells' thickness: the domains of the smallest gap") : tr("the wires' area: the domains of the smallest gap"));
        if (a_1d) {
            a_s.setGlobal(p + QStringLiteral("Lstack"), a_L, Dim::length(), tr("the stack's thickness: the physics' line, its padding too"));
            a_s.setGlobal(p + QStringLiteral("Lnorm"), a_spec.per, Dim::length(),
                          tr("what the absorption is per: the wells' thickness, the stack's or a length (the step's Absorption per)"));
        }
        if (k < a_spec.dnTE.size()) {
            a_s.setGlobal(p + QStringLiteral("dnTE"), a_spec.dnTE[k], Dim(),
                          tr("the wells' index the absorption in the window makes, TE (Kramers-Kronig): between two fields, "
                             "electrorefraction's change"));
            a_s.setGlobal(p + QStringLiteral("dnTM"), a_spec.dnTM[k], Dim(), tr("the wells' index the absorption in the window makes, TM"));
            a_s.setGlobal(p + QStringLiteral("dnTEtail"), a_spec.dnTEtail[k], Dim(),
                          tr("what the absorption above the window would add to %1dnTE were it its top's all the way: between two "
                             "fields their difference what Δn leaves out").arg(p));
            a_s.setGlobal(p + QStringLiteral("dnTMtail"), a_spec.dnTMtail[k], Dim(), tr("the same of %1dnTM").arg(p));
        }
        if (k < a_spec.farTE.size()) {
            a_s.setGlobal(p + QStringLiteral("alphaTEfar"), a_spec.farTE[k], perMetre,
                          tr("what the transitions more than 5 half widths above the photon add to %1alphaTE: their lines' tails "
                             "alone - a Lorentzian's reach far, a Gaussian's and a sech's hardly").arg(p));
            a_s.setGlobal(p + QStringLiteral("alphaTMfar"), a_spec.farTM[k], perMetre, tr("the same of %1alphaTM").arg(p));
        }
        a_s.setGlobal(p + QStringLiteral("Fw"), a_spec.field, Dim::of(1, 1, -3, -1), tr("the field across the wells (their band edge's slope)"));
        a_s.setGlobal(p + QStringLiteral("linewidth"), a_spec.gamma, energy, tr("the broadening: its half width at half maximum"), QStringLiteral("eV"));
        if (k < a_spec.rsp.size()) {
            a_s.setGlobal(p + QStringLiteral("rsp"), a_spec.rsp[k], Dim::of(-5, -1, 1), tr("spontaneous emission per volume of the wells, time and photon energy"),
                          QStringLiteral("1/(s*cm^3*eV)"));
            a_s.setGlobal(p + QStringLiteral("Rsp"), a_spec.Rsp, Dim::of(-3, 0, -1), tr("spontaneous emission per volume of the wells and time, over the energies"),
                          QStringLiteral("1/(s*cm^3)"));
            a_s.setGlobal(p + QStringLiteral("gspTE"), a_spec.gspTE[k], perMetre,
                          tr("the spontaneous emission coefficient, TE: the gain's with f_c (1 - f_v) for f_c - f_v"));
            a_s.setGlobal(p + QStringLiteral("gspTM"), a_spec.gspTM[k], perMetre, tr("the spontaneous emission coefficient, TM"));
        }
        if (a_spec.excitons) {
            a_s.setGlobal(p + QStringLiteral("Eb"), a_spec.Eb, energy, tr("the e1-hh1 exciton's binding energy: its 1s, variational - or, its subbands coupled, its edge less the strongest line below it"), QStringLiteral("eV"));
            a_s.setGlobal(p + QStringLiteral("Ex"), a_spec.Ex, energy, tr("the e1-hh1 exciton's line: the transition less its binding"), QStringLiteral("eV"));
            a_s.setGlobal(p + QStringLiteral("aX"), a_spec.radius, Dim::length(), tr("the e1-hh1 exciton's radius in the wells' plane, λ (its subbands coupled: Bastard's λ of its line's |ψ(0)|², 2/(πλ²))"));
            a_s.setGlobal(p + QStringLiteral("gX"), a_spec.gammaX, Dim(),
                          tr("the e1-hh1 exciton's γ: its electron and hole drawn together along the axis (0: Bastard's; its subbands coupled: none)"));
        }
        if (a_spec.carriers) {
            a_s.setGlobal(p + QStringLiteral("Fc"), a_spec.Fc, energy, tr("the electrons' quasi-Fermi level"), QStringLiteral("eV"));
            a_s.setGlobal(p + QStringLiteral("Fv"), a_spec.Fv, energy, tr("the holes' quasi-Fermi level"), QStringLiteral("eV"));
            a_s.setGlobal(p + QStringLiteral("dF"), a_spec.Fc - a_spec.Fv, energy,
                          tr("the quasi-Fermi levels' separation: gain below it (Bernard and Duraffourg)"), QStringLiteral("eV"));
        }
    }
}

} // namespace

double kramersKronig(const std::vector<double>& x, const std::vector<double>& alpha, double E, double aE)
{
    if (x.size() < 2) return 0;
    const double lo = x.front(), hi = x.back();
    // (At E and within rounding of it - an energy given twice, the photon's
    // beside its own - the integrand's limit, α'(E)/2E, α' by the energies
    // either side: not 0, nor a quotient of two roundings.)
    constexpr double Rounding = 1e-9;
    const std::size_t at = std::size_t(std::lower_bound(x.begin(), x.end(), E * (1 - Rounding)) - x.begin());
    std::size_t past = at;
    while (past < x.size() && std::abs(x[past] - E) <= Rounding * E) ++past;
    double limit = 0;
    if (at > 0 && past < x.size()) limit = (alpha[past] - alpha[at - 1]) / (x[past] - x[at - 1]);
    else if (at > 0) limit = (aE - alpha[at - 1]) / (E - x[at - 1]);
    else if (past < x.size()) limit = (alpha[past] - aE) / (x[past] - E);
    if (E > 0) limit /= 2 * E;
    double sum = 0;
    for (std::size_t k = 1; k < x.size(); ++k) {
        auto f = [&](std::size_t i) { return i >= at && i < past ? limit : (alpha[i] - aE) / (x[i] * x[i] - E * E); };
        sum += 0.5 * (f(k) + f(k - 1)) * (x[k] - x[k - 1]);
    }
    // P∫_lo^hi dE'/(E'² - E²) = ln|((hi - E)(lo + E))/((hi + E)(lo - E))| / 2E.
    if (E > 0 && std::abs(hi - E) > 1e-12 * Q && std::abs(lo - E) > 1e-12 * Q)
        sum += aE * std::log(std::abs((hi - E) * (lo + E) / ((hi + E) * (lo - E)))) / (2 * E);
    return C0 * Hbar / M_PI * sum;
}

std::unique_ptr<Problem> makeQuantumProblem(Solution& solution, int field)
{
    return std::make_unique<QuantumProblem>(solution, field);
}

QuantumLayers quantumLayers(Solution& solution, int field)
{
    QuantumLayers out;
    QuantumProblem p(solution, field);
    if (!p.setup(&out.error)) return out;
    out.T = p.a_T;
    out.F = p.a_F;
    out.h1d = p.a_h1d;
    out.valence = p.a_valence;
    out.axis = p.a_axis;
    out.line = p.a_1d;
    out.potential = p.a_V.isValid();
    for (const Band& b : p.a_bands) {
        QuantumLayer l;
        l.on = b.on;
        l.user = b.user;
        l.wurtzite = b.wz;
        l.valleys = !b.valleys.empty();
        l.Ec = b.Ec0;
        l.Ev = b.Ev0;
        l.me = b.me;
        l.g1 = b.g1;
        l.g2 = b.g2;
        l.g3 = b.g3;
        l.delta = b.delta;
        l.Ep = b.Ep;
        l.nr = b.nr;
        l.epsr = b.epsr;
        l.exx = b.strain;
        l.a0 = b.a0;
        l.ac = b.ac;
        l.av = b.av;
        l.b = b.bDef;
        l.C11 = b.c11;
        l.C12 = b.c12;
        out.layers.push_back(l);
    }
    return out;
}

QuantumState quantumAt(PointEvaluator& e, int fi)
{
    QuantumState s;
    const Field& f = e.solution().fields()[std::size_t(fi)];
    if (!f.fixed || f.fixed->size() < std::size_t(Header)) return s;
    const int t = e.triangle(), d = e.domain();
    if (t < 0 || d < 0 || std::size_t(d) >= f.domains.size() || !f.domains[std::size_t(d)]) return s;
    const std::vector<double>& fx = *f.fixed;
    const bool line = fx[0] == 1;
    const std::size_t n1 = std::size_t(fx[6]), nt = std::size_t(fx[8]), nd = std::size_t(fx[9]);
    const std::size_t triBase = std::size_t(Header) + n1, domBase = triBase + 9 * nt;
    if (std::size_t(t) >= nt || std::size_t(d) >= nd || fx.size() < domBase + 4 * nd) return s;
    s.defined = true;
    const double l0 = 1 - e.xi() - e.eta(), l1 = e.xi(), l2 = e.eta();
    double* edge[3] = {&s.Ec, &s.Ehh, &s.Elh};
    for (int c = 0; c < Carriers; ++c) {
        const std::size_t b = triBase + std::size_t(9 * t + 3 * c);
        *edge[c] = l0 * fx[b] + l1 * fx[b + 1] + l2 * fx[b + 2];
    }
    s.me = fx[domBase + std::size_t(4 * d)];
    s.mhh = fx[domBase + std::size_t(4 * d + 1)];
    s.mlh = fx[domBase + std::size_t(4 * d + 2)];
    s.strain = fx[domBase + std::size_t(4 * d + 3)];
    s.drawn = fx[7];
    if (f.extra.size() < std::size_t(2 * Carriers)) return s;
    for (int c = 0; c < Carriers; ++c) {
        s.E[c] = f.extra[std::size_t(c)];
        s.peak[c] = f.extra[std::size_t(Carriers + c)];
    }
    if (line) {
        // Along the line: its quadratic elements at the point's projection.
        const QPointF S(fx[2], fx[3]), E(fx[4], fx[5]);
        const double L = std::hypot(E.x() - S.x(), E.y() - S.y());
        const QPointF p = e.point() - S;
        const double z = L > 0 ? (p.x() * (E.x() - S.x()) + p.y() * (E.y() - S.y())) / L : -1;
        if (z < 0 || z > L || n1 < 3 || f.extra.size() < std::size_t(2 * Carriers) + Carriers * n1) return s;
        const double* zs = fx.data() + Header;
        const std::ptrdiff_t at = std::upper_bound(zs, zs + n1, z) - zs;
        std::size_t el = std::size_t(std::max<std::ptrdiff_t>(0, at - 1)) / 2;
        el = std::min(el, (n1 - 1) / 2 - 1);
        const double z0 = zs[2 * el], z2 = zs[2 * el + 2];
        const double xi = z2 > z0 ? std::clamp((z - z0) / (z2 - z0), 0.0, 1.0) : 0;
        const double N[3] = {(1 - xi) * (1 - 2 * xi), 4 * xi * (1 - xi), xi * (2 * xi - 1)};
        for (int c = 0; c < Carriers; ++c) {
            if (!std::isfinite(s.E[c])) continue;
            const double* v = f.extra.data() + 2 * Carriers + std::size_t(c) * n1 + 2 * el;
            s.psi[c] = N[0] * v[0] + N[1] * v[1] + N[2] * v[2];
        }
    } else {
        for (int c = 0; c < Carriers; ++c) {
            if (!std::isfinite(s.E[c])) continue;
            double u = 0, gx = 0, gy = 0;
            e.component(fi, c, u, gx, gy);
            s.psi[c] = u;
        }
    }
    return s;
}

} // namespace qucs_s::fem::detail
