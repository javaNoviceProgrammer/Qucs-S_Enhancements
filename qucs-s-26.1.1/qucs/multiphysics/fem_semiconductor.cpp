/*
 * fem_semiconductor.cpp - semiconductors: the drift-diffusion equations
 *                         (Poisson's for the potential, continuity for
 *                         electrons and holes) on linear triangles, each
 *                         edge's carrier current Scharfetter and Gummel's
 *                         weighed as the Laplacian weighs it (edge-averaged
 *                         finite elements: an M-matrix on a Delaunay mesh);
 *                         solved together by Newton's method from
 *                         equilibrium, the contacts' voltages raised in
 *                         steps; in time (BDF), and small-signal about a
 *                         solution (the contacts' admittance matrix); a
 *                         contact's current weighed over the device where
 *                         it is resolved, not taken from its own rows
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_problem.h"
#include "fem_geometry.h"
#include "fem_mesh.h"

#include <Eigen/SparseCholesky>
#include <Eigen/QR>
#include <Eigen/SparseLU>

#include <algorithm>
#include <cmath>
#include <complex>
#include <map>
#include <optional>
#include <tuple>

namespace qucs_s::fem::detail {

namespace {

constexpr double Q = 1.602176634e-19;    // C
constexpr double KB = 1.380649e-23;      // J/K
const double Inf = std::numeric_limits<double>::infinity();
// The field's extra: Vt, T, ni_ref, then each triangle's µn, µp, Ec - Ei, Eg,
// ni, τn, τp, Et, Cn, Cp, B, δ, incomplete ionization's n1d, gd, p1a, ga,
// Fermi-Dirac's Nc, Nv (0 for Boltzmann's), and the carriers' Joule heat as
// solved (W/m³) (NaN where it is no semiconductor); its values V, n, p, Nd,
// Na, θn, θp by node (θ with Fermi-Dirac's degeneracy in it, as solved).
constexpr int ExtraHead = 3, PerTriangle = 19;

/// Bernoulli's function x / (e^x - 1) and its derivative, safe at 0 and far
/// from it.
double bern(double x)
{
    if (std::abs(x) < 1e-5) return 1 - x / 2 + x * x / 12;
    if (x > 600) return x * std::exp(-x);
    if (x < -600) return -x;
    return x / std::expm1(x);
}

double dbern(double x)
{
    if (std::abs(x) < 1e-5) return -0.5 + x / 6;
    if (x > 600) return (1 - x) * std::exp(-x);
    if (x < -600) return -1;
    const double em = std::expm1(x);
    return (em - x * (em + 1)) / (em * em);
}

/// A domain's semiconductor (or insulator) as solved.
struct Medium {
    bool semiconductor = false;
    double epsr = 1;
    double Eg = 0, chi = 0;          // J
    double Nc = 0, Nv = 0;           // 1/m³
    double mun = 0, mup = 0;         // m²/(V s): the lattice's (most)
    bool dopingMobility = false;     // Caughey and Thomas's
    double munMin = 0, nRefN = 1, alphaN = 1, mupMin = 0, nRefP = 1, alphaP = 1;
    bool saturation = false;         // high fields: Caughey and Thomas's too
    double vsatn = 1e5, vsatp = 8e4, betan = 2, betap = 1;
    bool wholeField = false;         // saturated by |E|, not by the force along the current
    double zetan = 2.5, zetap = 2.2;   // the mobilities' (TL/300 K)^-ζ
    // Lombardi's scattering at an insulator's surface: µac = B/E⊥ + C (N/1
    // cm⁻³)^λ / (T E⊥^⅓) (C in cm, V, s and K), µsr = δ/E⊥², E⊥ the field
    // across the nearest interface, each faded as e^(-d/lcrit) away from it.
    bool surface = false;
    double sBn = 0, sCn = 0, sLn = 0, sDn = 0, sBp = 0, sCp = 0, sLp = 0, sDp = 0, lcrit = 1e-8;
    // Slotboom's band gap narrowing: ΔEg = E (ln(N/Nref) + √(ln²(N/Nref) +
    // C)) (J), N the donors and acceptors - ni raised by e^(ΔEg/2kT), the
    // bands closing on the intrinsic level equally.
    bool bgn = false;
    double bgnE = 0, bgnN = 1, bgnC = 0.5;
    // Incomplete ionization: Nd⁺ = Nd/(1 + gd n/n1d), n1d = Nc e^(-ΔEd/kT),
    // Na⁻ = Na/(1 + ga p/p1a), p1a = Nv e^(-ΔEa/kT).
    bool incomplete = false;
    double n1d = Inf, gd = 2, p1a = Inf, ga = 4;
    // Fermi-Dirac's statistics: n = Nc ℱ1/2(η), not Nc e^η.
    bool fermi = false;
    bool srh = false;
    double taun = Inf, taup = Inf, Et = 0;   // Et: the trap's level above the intrinsic one (J)
    double Cn = 0, Cp = 0, B = 0;
    std::vector<Expression> generation;
    std::vector<int> optical;        // the Optical Generation features on it (a_optical's)
    double ni = 0, dEc = 0;          // the intrinsic density; Ec - Ei (J)
    // Impact ionization: Chynoweth's α = a e^(-(b/E)^β) (1/m, V/m), each
    // carrier's below E0 and above it (van Overstraeten and de Man's holes).
    bool ionization = false, ionizationCurrents = false;
    double an = 0, bn = 0, an2 = 0, bn2 = 0, ap = 0, bp = 0, ap2 = 0, bp2 = 0, E0 = Inf, ionBetaN = 1, ionBetaP = 1;
};

/// The Fermi-Dirac integral of order 1/2, normalized - ℱ(η) = (2/√π) ∫0^∞
/// √ε/(1 + e^(ε-η)) dε, e^η where the carriers are few - and its derivative
/// ℱ-1/2: tabulated once from 20 kT below the band's edge to 100 above at
/// 1/32 (Simpson's rule in √ε), cubic between (ℱ to 2e-9, its derivative
/// to 2e-7, against a quadrature a hundred times finer); Boltzmann's two
/// terms below, Sommerfeld's above.
struct FermiHalf {
    static constexpr double Lo = -20, Hi = 100, Step = 1.0 / 32;
    std::vector<double> f, d;
    FermiHalf()
    {
        const int n = int((Hi - Lo) / Step + 0.5) + 1;
        f.resize(std::size_t(n));
        d.resize(std::size_t(n));
        const double c = 2 / std::sqrt(M_PI);
        for (int k = 0; k < n; ++k) {
            const double eta = Lo + k * Step;
            // t = √ε: ℱ = (4/√π) ∫ t²/(1 + e^(t²-η)) dt, ℱ' = (2/√π) ∫ dt/(1 + e^(t²-η)).
            const double T = std::sqrt(std::max(eta, 0.0) + 60);
            const int m = 4000;
            const double h = T / m;
            double sf = 0, sd = 0;
            for (int j = 0; j <= m; ++j) {
                const double t = j * h, w = (j == 0 || j == m) ? 1 : (j % 2 ? 4 : 2);
                const double x = t * t - eta;
                const double occupied = x > 0 ? std::exp(-x) / (1 + std::exp(-x)) : 1 / (1 + std::exp(x));
                sf += w * t * t * occupied;
                sd += w * occupied;
            }
            f[std::size_t(k)] = 2 * c * sf * h / 3;
            d[std::size_t(k)] = c * sd * h / 3;
        }
    }
    void at(double eta, double* F, double* dF) const
    {
        if (eta < Lo) {
            const double e = std::exp(eta);
            *F = e * (1 - e / (2 * std::sqrt(2.0)));
            *dF = e * (1 - e / std::sqrt(2.0));
            return;
        }
        if (eta >= Hi) {
            const double c = 4 / (3 * std::sqrt(M_PI)), p2 = M_PI * M_PI;
            *F = c * std::pow(eta, 1.5) * (1 + p2 / (8 * eta * eta) + 7 * p2 * p2 / (640 * std::pow(eta, 4)));
            *dF = c * 1.5 * std::sqrt(eta) * (1 - p2 / (24 * eta * eta) - 7 * p2 * p2 / (384 * std::pow(eta, 4)));
            return;
        }
        const double u = (eta - Lo) / Step;
        const int k = std::min(int(u), int(f.size()) - 2);
        const double s = u - k, h = Step;
        const double f0 = f[std::size_t(k)], f1 = f[std::size_t(k + 1)], d0 = d[std::size_t(k)], d1 = d[std::size_t(k + 1)];
        *F = (2 * s * s * s - 3 * s * s + 1) * f0 + (s * s * s - 2 * s * s + s) * h * d0 + (-2 * s * s * s + 3 * s * s) * f1 + (s * s * s - s * s) * h * d1;
        *dF = (6 * s * s - 6 * s) / h * f0 + (3 * s * s - 4 * s + 1) * d0 + (-6 * s * s + 6 * s) / h * f1 + (3 * s * s - 2 * s) * d1;
    }
    /// η where ℱ(η) = \a u (Newton's in ln ℱ, from Joyce and Dixon's start
    /// or Sommerfeld's).
    double inverse(double u) const
    {
        double eta = u < 1 ? std::log(u) + u / std::sqrt(8.0) : std::pow(0.75 * std::sqrt(M_PI) * u, 2.0 / 3);
        for (int k = 0; k < 60; ++k) {
            double F = 0, dF = 0;
            at(eta, &F, &dF);
            const double step = (std::log(F) - std::log(u)) * F / dF;
            eta -= step;
            if (std::abs(step) < 1e-13 * std::max(1.0, std::abs(eta))) break;
        }
        return eta;
    }
};

const FermiHalf& fermiHalf()
{
    static const FermiHalf table;
    return table;
}

/// Chynoweth's ionization rate at field \a E (V/m) in its range, and its
/// change with E.
void ionizationRate(double E, double a, double b, double a2, double b2, double E0, double beta, double* alpha, double* dalpha)
{
    if (!(E > 1e3)) {
        *alpha = *dalpha = 0;
        return;
    }
    const bool high = E > E0;
    const double r = std::pow((high ? b2 : b) / E, beta);
    *alpha = (high ? a2 : a) * std::exp(-r);
    *dalpha = *alpha * beta * r / E;
}

/// Light absorbed from a mode, made electron-hole pairs: the power fed P0
/// (of t in time), each photon absorbed across the gap a pair (η of them).
struct OpticalSource {
    QString mode;                    // the mode analysis's tag
    int number = 1;                  // which of its modes (1: the highest index)
    Expression absorbed;             // its <tag>.Qgap (W/m³, the mode at 1 W)
    Expression power;                // P0's
    double P0 = 0, eta = 1, RL = 50;
};

struct Contact {
    enum Kind { Ohmic, Schottky, Gate };
    QString name;
    Kind kind = Ohmic;
    Expression V0;
    double phiB = 0;                 // a Schottky contact's barrier (J)
    double vn = 1e5, vp = 1e5;       // its recombination velocities (m/s)
    double phim = 0;                 // a gate's work function (J)
    std::vector<int> nodes;
    std::vector<double> length;      // by node: the contact's area there (m²: a length times the thickness)
    double applied = 0;              // the voltage the solution is at (V)
    double current = 0, charge = 0;  // A into the device, C on the metal
};

class SemiconductorProblem : public Problem
{
public:
    SemiconductorProblem(Solution& solution, int field) : Problem(solution, field) {}

    bool setup(QString* error) override;
    void initialize() override;
    bool solve(const SolveOptions& options, QString* error) override;
    void setGlobals() override;
    bool hasMass() const override { return true; }
    bool harmonic() const override { return true; }

private:
    struct Edge {
        int i = -1, j = -1;
        double w = 0;                // Σ cot weights (m², the thickness in)
        double wn = 0, wp = 0;       // times the elements' mobilities (the lattice's, by doping)
        double h = 0;                // its length (m)
        int domain = -1;             // a semiconductor's it is in (one that ionizes, if one does)
        double tx = 0, ty = 0;       // its direction, i to j
    };
    int nodes() const { return a_n; }
    double doping(int i) const { return a_Nd[std::size_t(i)] - a_Na[std::size_t(i)]; }
    /// Equilibrium's carriers at a node of net doping \a C (no applied
    /// voltage), and their potential in Vt above the Fermi level's, v.
    void neutral(int i, double* n, double* p, double* v = nullptr) const;
    /// The contacts' voltages at the solution's time.
    bool targets(std::vector<double>& out, QString* error);
    /// The values a Dirichlet node takes (V, n, p) at the contacts' applied voltages.
    void contactValues(const std::vector<double>& applied);
    /// F(x) and (\a jac) its Jacobian - interior rows; the contacts' rows as
    /// they are (their reactions), Dirichlet rows replaced in \a system.
    void assemble(const Eigen::VectorXd& x, double mass, const std::vector<double>* history, Eigen::VectorXd& F,
                  std::vector<Eigen::Triplet<double>>* jac) const;
    /// Newton's method from \a x at the contacts' values set; how many steps
    /// in \a steps.
    bool newton(Eigen::VectorXd& x, double mass, const std::vector<double>* history, int maxSteps, int* steps, QString* why);
    /// Poisson's equation alone, the carriers at equilibrium with a Fermi
    /// level of -q \a fermi, the contacts at \a applied; from \a x's
    /// potential (\a warm) or charge neutrality's.
    bool equilibriumPoisson(Eigen::VectorXd& x, double fermi, const std::vector<double>& applied, bool warm, QString* why);
    /// Whether the contacts that hold the carriers are at one voltage (in
    /// \a fermi) and nothing makes carriers: no current flows, and the
    /// carriers are at equilibrium - Poisson's equation alone is exact.
    bool atEquilibrium(const std::vector<double>& goal, double* fermi) const;
    /// At rest: from where the solution is to the contacts' voltages, in steps.
    bool stationary(QString* why);
    bool smallSignal(double frequency, QString* why);
    /// At equilibrium: the gates' quasi-static capacitance - the carriers
    /// following, their Fermi level held at \a fermi - from Poisson's equation
    /// linearized: the low-frequency limit, well conditioned (the small-signal
    /// system is not at 1 µHz: a MOS capacitor's inversion layer is held by
    /// almost nothing).
    void quasiStatic(const Eigen::VectorXd& x, double fermi, const std::vector<double>& applied);
    /// Poisson's equation with the carriers at equilibrium (Fermi level
    /// \a fermi): its residual on the potential's unknowns (\a Fv, by \a
    /// vIndex) and Jacobian (\a J, contacts' rows as assembled when \a
    /// keepContacts, else fixed).
    void equilibriumSystem(const Eigen::VectorXd& x, double fermi, const std::vector<int>& local, const std::vector<int>& vIndex,
                           const std::vector<char>& fixed, bool keepContacts, Eigen::VectorXd& Fv, Eigen::SparseMatrix<double>& J) const;
    void store(const Eigen::VectorXd& x);
    Eigen::VectorXd stateVector() const;
    /// The contacts' currents and charges at \a x (at equilibrium, \a
    /// still: no current, exactly).
    void terminalValues(const Eigen::VectorXd& x, double mass, const std::vector<double>* history, bool still = false);
    /// By contact (empty for a gate): a weight over the nodes, 1 on its own,
    /// 0 on the other contacts' that hold carriers, between them as the
    /// carriers' conductance at \a x has it - flat where they are many. A
    /// contact's current is the edges' currents times the weight's change
    /// along them: the same as its rows' reaction where the equations hold,
    /// but not the difference of a 10^19 cm^-3 contact's two large fluxes
    /// (each edge's ~100 A per metre, its last digits pA).
    std::vector<std::vector<double>> currentWeights(const Eigen::VectorXd& x);
    /// The edge mobilities at the fields of \a x (high fields lower them).
    void updateMobilities(const Eigen::VectorXd& x);
    /// Along edge \a e at \a x: the electrons' and the holes' potential
    /// steps in Vt - the potential's, and across a heterojunction the bands'
    /// offsets (θn, θp: the intrinsic level's, and the densities of states'
    /// as ni has them), and Fermi-Dirac's degeneracy (ln γ, last assembled).
    void edgeSteps(const Edge& e, const Eigen::VectorXd& x, double* dn, double* dp) const
    {
        const double dV = x(a_iV[std::size_t(e.j)]) - x(a_iV[std::size_t(e.i)]);
        const std::size_t i = std::size_t(e.i), j = std::size_t(e.j);
        *dn = (dV + a_thetaN[j] - a_thetaN[i]) / a_Vt + a_lgN[j] - a_lgN[i];
        *dp = (dV + a_thetaP[j] - a_thetaP[i]) / a_Vt - a_lgP[j] + a_lgP[i];
    }

    int a_n = 0;                       // nodes (P1 degrees of freedom)
    double a_T = 300, a_Vt = 0.025852;
    double a_scale = 1;                // a carrier scale (1/m³): the largest doping or ni
    std::vector<Medium> a_media;       // by domain
    std::vector<char> a_elemSemi;      // by triangle: of a semiconductor in the physics
    std::vector<char> a_elemOn;        // in the physics
    std::vector<int> a_nodeDomain;     // a node's semiconductor domain (-1: none)
    std::vector<double> a_box;         // by node: its share of semiconductor (m³)
    std::vector<double> a_Nd, a_Na, a_ni, a_n1, a_p1;
    std::vector<double> a_gap;         // by node: the band gap's narrowing (J)
    bool a_incomplete = false;
    // Fermi-Dirac's statistics by node, at the carriers last assembled: ln γ
    // (n = γ Nc e^η, γ = ℱ1/2(η)/e^η - 1 where they are few), its change
    // with the carriers, and ℱ'/ℱ (δn = n ℱ'/ℱ δη); Boltzmann's 0, 0, 1. Each
    // carrier's band offset θ takes Vt ln γ: the flux as Boltzmann's, its
    // diffusion the generalized Einstein relation's.
    bool a_fermi = false;
    mutable std::vector<double> a_lgN, a_lgP, a_dlgN, a_dlgP, a_rN, a_rP;
    // Schrödinger-Poisson: by node, the quantum potentials Λn, Λp (V) taken
    // into the bands' offsets - θn + Λn, θp - Λp: the carriers there e^(Λ/Vt)
    // Boltzmann's, as the states have them; the offsets without them; the
    // nodes in the regions asked, and the states' carriers' expressions.
    std::vector<double> a_lamN, a_lamP, a_thetaN0, a_thetaP0;
    std::vector<int> a_quantumNodes;
    Expression a_quantumN, a_quantumP;
    std::vector<Eigen::VectorXd> a_andX, a_andG;   // Anderson's history: Λ/Vt and its steps
    bool a_quantumElectrons = false, a_quantumHoles = false;
    double quantumCorrection(QString* error) override;
    // Electrothermal: the lattice temperature for the mobilities (another
    // physics', ht.T), each triangle's factors from it at each solve.
    Expression a_TL;
    void latticeTemperature();
    void degeneracy(const Eigen::VectorXd& x) const;
    /// Node \a i's carriers at v, the potential in Vt above its Fermi
    /// level's (Boltzmann's n = ni e^v): Fermi-Dirac's where asked; their
    /// changes with v.
    void carriersAt(int i, double v, double* n, double* p, double* dn, double* dp) const;
    /// Node \a i's ionized donors and acceptors at \a n, \a p (1/m³) - the
    /// carriers' Boltzmann densities, n/γn and p/γp: the dopants filled by
    /// the Fermi level whatever the carriers' statistics - and their changes
    /// with them.
    void ionized(int i, double n, double p, double* nd, double* dnd, double* na, double* dna) const;
    // By node, across heterojunctions: the intrinsic level's offset from the
    // first semiconductor's (V: Ei = -q (V + δ)), and the electrons' and
    // holes' band potentials θn = δ + Vt ln(ni/ni_ref), θp = δ - Vt ln(ni/ni_ref)
    // - n = ni_ref exp((V + θn - φn)/Vt), p = ni_ref exp((φp - V - θp)/Vt).
    std::vector<double> a_delta, a_thetaN, a_thetaP;
    double a_refNi = 1;
    std::vector<std::array<double, 9>> a_K;   // by triangle: ε0 εr ∫∇φi·∇φj (weighted)
    std::vector<Edge> a_edges;
    // By triangle of a semiconductor: its low-field mobilities, its edges
    // (k to k+1) and their cot weights, its shape functions' gradients.
    struct ElementMobility {
        double mun = 0, mup = 0;
        int edge[3] = {-1, -1, -1};
        double cot[3] = {0, 0, 0};
        double gx[3] = {0, 0, 0}, gy[3] = {0, 0, 0};
        int domain = -1;
        double volume = 0;           // its area times the thickness (m³)
        double N = 0;                // its donors and acceptors (1/m³)
        // Surface scattering's: the nearest interface's normal, e^(-d/lcrit).
        double nx = 0, ny = 0, damp = 0;
        // The lattice's temperature's: (TL/300 K)^-ζ of each carrier's.
        double tn = 1, tp = 1;
    };
    std::vector<ElementMobility> a_elements;
    /// Triangle \a t's mobilities at \a x (m²/(V s)): its low-field ones,
    /// scattered at the nearest insulator's surface by the field across it,
    /// saturated by the force along each carrier's current - the low-field
    /// ones alone without \a x.
    void elementMobilities(int t, const Eigen::VectorXd* x, double* mun, double* mup) const;
    /// The edges' weights from the elements' mobilities at \a x (when there
    /// is one, and the model asks).
    void edgeWeights(const Eigen::VectorXd* x);
    std::vector<int> a_iV, a_iN, a_iP; // by node: unknowns (-1 none)
    int a_unknowns = 0;
    std::vector<Contact> a_contacts;
    std::vector<int> a_nodeContact;    // by node (-1 none)
    std::vector<double> a_bcV, a_bcN, a_bcP;   // by node: a Dirichlet node's values
    std::vector<std::pair<int, Expression>> a_surface;   // node, charge density's expression
    std::vector<double> a_surfaceLength;                 // by entry: area (m²)
    // Interface traps, by level at a node: its share of the interface (m²),
    // density (1/m²), n1 and p1 (1/m³: ni e^(±(Et - Ei)/kT)), cn and cp (σ
    // vth, m³/s), and whether acceptor-like.
    struct TrapLevel {
        int node = -1;
        double area = 0, N = 0, n1 = 0, p1 = 0, cn = 0, cp = 0;
        bool acceptor = true;
    };
    std::vector<TrapLevel> a_traps;
    /// Level \a t at Boltzmann's carriers \a n, \a p: its occupancy f, its
    /// rate 1/τ = cn (n + n1) + cp (p + p1), the pairs it recombines per area
    /// U, and their changes with n and p.
    struct TrapState {
        double f, D, U, dfdn, dfdp, dUdn, dUdp;
    };
    static TrapState trapState(const TrapLevel& t, double n, double p);
    std::vector<std::pair<int, std::array<double, 3>>> a_nodePoint;   // a node's triangle and reference point
    std::vector<double> a_generation;  // by node (1/(m³ s)), at the last solve
    std::vector<OpticalSource> a_optical;
    double a_Gop = 0, a_Pin = 0;       // the optical generation's pairs per second, the light fed (W)
    std::vector<std::complex<double>> a_Hop;   // small-signal: each contact's current, the light modulated wholly (A)
    bool a_saturation = false, a_lombardi = false;
    bool a_alongCurrent = false;       // a saturation driven by the field along the current
    double a_refDEc = 0, a_refChi = 0; // the first semiconductor's: Ec - Ei, χ (J)
    // Impact ionization: whether its pairs are in the equations; the
    // ionization integrals along the field line through the highest field
    // (electrons', holes'), that field (V/m), the line's length (m).
    bool a_ionization = false, a_ionizationAny = false;
    double a_In = NaN, a_Ip = NaN, a_Emax = NaN, a_ionLength = NaN;
    std::vector<std::array<int, 3>> a_neighbors;   // by triangle: across its edge k to k+1 (-1: none)
    /// The ionization integrals at \a x (a_In, a_Ip...).
    void ionizationIntegrals(const Eigen::VectorXd& x);
    QString a_initError;
    int a_clamped = 0;
    // Small-signal: the admittance matrix by contacts (S), at a frequency.
    std::vector<std::vector<std::complex<double>>> a_Y;
    double a_frequency = 0;
    // At equilibrium: the gates' quasi-static capacitance matrix (F), by gate.
    std::vector<std::vector<double>> a_C0;
    std::vector<int> a_gates;
    // The weights' system (its pattern the mesh's: analysed once).
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> a_weightSolver;
    int a_weightSize = -1;
};

/// A band energy (eV, or V as eV per electron, or a plain number in eV)
/// in joules; NaN and why when it is not a constant.
double energyOf(const QString& text, const Scope& scope, QString* why)
{
    const Expression e = Expression::compile(text, scope);
    if (!e.isValid() || !e.isConstant()) {
        *why = e.isValid() ? tr("%1 must be a constant").arg(text) : e.error();
        return NaN;
    }
    if (e.dim() == Dim::of(2, 1, -2)) return e.constant();
    return e.constant() * Q;   // V or a plain number: eV
}

bool SemiconductorProblem::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field& f = field();
    const Model& model = a_s.model();
    const Topology& topo = a_s.topology();
    const Mesh& mesh = a_s.mesh();
    const Space& sp = *f.space;
    const Scope& constants = a_s.parameters().scope;
    QString why;
    a_n = sp.size();
    const QVector<int> domains = resolveSelection(topo, model, Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(topo.domains.size(), 0);
    for (int d : domains) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    if (sp.order() != 1) problem(tr("%1: its elements are linear (Scharfetter and Gummel's currents)").arg(a_node->name()));
    {
        const std::optional<double> t = evaluateConstant(a_node->text(QStringLiteral("T")), constants, Dim::temperature(), &why);
        if (!t || !(*t > 0)) problem(tr("%1: its temperature: %2").arg(a_node->name(), t ? tr("it must be more than 0") : why));
        else a_T = *t;
    }
    a_Vt = KB * a_T / Q;
    {
        const QString tl = a_node->text(QStringLiteral("TL")).trimmed();
        a_TL = tl.isEmpty() ? Expression() : compile(tl, *a_node, QStringLiteral("TL"));
    }
    auto number = [&](const QString& text, const Dim& dim, const QString& what) {
        const std::optional<double> v = evaluateConstant(text, constants, dim, &why);
        if (!v) {
            problem(tr("%1: %2").arg(what, why));
            return NaN;
        }
        return *v;
    };
    const std::vector<const Node*> material = materials();
    a_media.assign(topo.domains.size(), Medium());
    std::vector<const Node*> modelOf(topo.domains.size(), nullptr);
    std::vector<const Node*> srhOf(topo.domains.size(), nullptr);
    std::vector<const Node*> augerOf(topo.domains.size(), nullptr), radiativeOf(topo.domains.size(), nullptr);
    std::vector<const Node*> impactOf(topo.domains.size(), nullptr);
    const Dim conc = Dim::of(-3), mobility = Dim::of(0, -1, 2, 1), velocity = Dim::of(1, 0, -1);
    struct DopingDef {
        const Node* node;
        QVector<int> domains;
    };
    std::vector<DopingDef> dopings;
    std::vector<const Node*> boundaryFeatures;
    std::vector<char> quantumOn(topo.domains.size(), 0);
    a_optical.clear();
    a_quantumN = a_quantumP = Expression();
    a_quantumElectrons = a_quantumHoles = false;
    for (const Node& feature : a_node->children) {
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k) continue;
        const QVector<int> picked = resolveSelection(topo, model, k->selection, feature.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(feature.name(), why));
        const QString t = feature.type;
        if (t == QLatin1String("semiconductor/model")) for (int d : picked) modelOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("semiconductor/srh")) for (int d : picked) srhOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("semiconductor/auger")) for (int d : picked) augerOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("semiconductor/radiative")) for (int d : picked) radiativeOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("semiconductor/impact")) for (int d : picked) impactOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("semiconductor/quantum")) {
            for (int d : picked) quantumOn[std::size_t(d)] = 1;
            a_quantumElectrons = feature.flag(QStringLiteral("electrons"));
            a_quantumHoles = feature.flag(QStringLiteral("holes"));
            if (a_quantumElectrons) a_quantumN = compile(feature.text(QStringLiteral("nq")), feature, QStringLiteral("nq"));
            if (a_quantumHoles) a_quantumP = compile(feature.text(QStringLiteral("pq")), feature, QStringLiteral("pq"));
        }
        else if (t == QLatin1String("semiconductor/doping")) dopings.push_back({&feature, picked});
        else if (t == QLatin1String("semiconductor/generation")) {
            const Expression g = compile(feature.text(QStringLiteral("G")), feature, QStringLiteral("G"));
            if (g.isValid())
                for (int d : picked) a_media[std::size_t(d)].generation.push_back(g);
        } else if (t == QLatin1String("semiconductor/optical")) {
            OpticalSource o;
            o.mode = feature.text(QStringLiteral("mode")).trimmed();
            if (o.mode.isEmpty()) o.mode = QStringLiteral("emw");
            if (a_s.fieldIndex(o.mode) < 0 || a_s.fields()[std::size_t(a_s.fieldIndex(o.mode))].type != QLatin1String("modes")) {
                problem(tr("%1: there is no mode analysis %2 (Electromagnetic Waves, Mode Analysis) to take its light from").arg(feature.name(), o.mode));
                continue;
            }
            // (What it reads noted: the mode's field.)
            o.absorbed = Expression::compile(o.mode + QStringLiteral(".Qgap"), a_s.pointScope());
            for (int slot : o.absorbed.variables())
                if (a_s.definitions()[std::size_t(slot)].field >= 0) a_reads.insert(a_s.definitions()[std::size_t(slot)].field);
            o.number = std::max(1, feature.integer(QStringLiteral("number")));
            o.power = compile(feature.text(QStringLiteral("P0")), feature, QStringLiteral("P0"), false);
            if (o.power.isValid() && o.power.isConstant()) o.P0 = o.power.constant();
            o.eta = number(feature.text(QStringLiteral("eta")), Dim(), feature.name());
            o.RL = number(feature.text(QStringLiteral("RL")), Dim::of(2, 1, -3, -2), feature.name());
            if (!(o.P0 >= 0) || !(o.eta >= 0 && o.eta <= 1)) problem(tr("%1: its power at least 0, its efficiency between 0 and 1").arg(feature.name()));
            const int index = int(a_optical.size());
            a_optical.push_back(o);
            for (int d : picked) a_media[std::size_t(d)].optical.push_back(index);
        } else if (k->selection == Level::Boundary) boundaryFeatures.push_back(&feature);
    }
    // Each domain's medium.
    bool referenced = false;
    for (int d : domains) {
        Medium& m = a_media[std::size_t(d)];
        const Node* mf = modelOf[std::size_t(d)];
        const Node* mat = material[std::size_t(d)];
        const QString object = topo.objectNames.value(topo.domains[std::size_t(d)].owner);
        const bool user = mf && mf->text(QStringLiteral("source")) == QLatin1String("user");
        auto text = [&](const QString& key) {
            if (user) return mf->text(key);
            return mat ? materialProperty(*mat, key, &constants) : QString();
        };
        const QString where = tr("domain %1 (%2)").arg(d + 1).arg(object);
        if (!user && !mat) {
            problem(tr("%1 has no material").arg(where));
            continue;
        }
        const QString eps = text(QStringLiteral("epsilonr"));
        if (eps.trimmed().isEmpty()) problem(tr("%1: its material has no relative permittivity").arg(where));
        else m.epsr = number(eps, Dim(), where);
        m.semiconductor = !text(QStringLiteral("Eg")).trimmed().isEmpty();
        if (!m.semiconductor) continue;
        m.Eg = energyOf(text(QStringLiteral("Eg")), constants, &why);
        if (!std::isfinite(m.Eg)) problem(tr("%1: its band gap: %2").arg(where, why));
        const QString chi = text(QStringLiteral("chi"));
        m.chi = chi.trimmed().isEmpty() ? 4.05 * Q : energyOf(chi, constants, &why);
        for (const auto& [key, member, dim] : {std::tuple{QStringLiteral("Nc"), &Medium::Nc, conc}, std::tuple{QStringLiteral("Nv"), &Medium::Nv, conc},
                                               std::tuple{QStringLiteral("mun"), &Medium::mun, mobility},
                                               std::tuple{QStringLiteral("mup"), &Medium::mup, mobility}}) {
            const QString v = text(key);
            if (v.trimmed().isEmpty()) problem(tr("%1: its material has no %2").arg(where, key));
            else m.*member = number(v, dim, where);
        }
        if (!(m.Nc > 0) || !(m.Nv > 0) || !(m.Eg > 0)) {
            problem(tr("%1: its band gap and effective densities of states must be more than 0").arg(where));
            continue;
        }
        m.ni = std::sqrt(m.Nc * m.Nv) * std::exp(-m.Eg / (2 * KB * a_T));
        m.dEc = m.Eg / 2 + KB * a_T / 2 * std::log(m.Nc / m.Nv);
        if (!referenced) {
            a_refDEc = m.dEc;
            a_refChi = m.chi;
            a_refNi = m.ni;
            referenced = true;
        }
        if (mf && mf->text(QStringLiteral("mobility")) == QLatin1String("doping")) {
            m.dopingMobility = true;
            m.munMin = number(mf->text(QStringLiteral("munmin")), mobility, where);
            m.nRefN = number(mf->text(QStringLiteral("nrefn")), conc, where);
            m.alphaN = number(mf->text(QStringLiteral("alphan")), Dim(), where);
            m.mupMin = number(mf->text(QStringLiteral("mupmin")), mobility, where);
            m.nRefP = number(mf->text(QStringLiteral("nrefp")), conc, where);
            m.alphaP = number(mf->text(QStringLiteral("alphap")), Dim(), where);
        }
        if (mf) {
            m.zetan = number(mf->text(QStringLiteral("zetan")), Dim(), where);
            m.zetap = number(mf->text(QStringLiteral("zetap")), Dim(), where);
            if (!std::isfinite(m.zetan) || !std::isfinite(m.zetap)) m.zetan = m.zetap = 0;
        }
        if (mf && mf->flag(QStringLiteral("saturation"))) {
            m.saturation = a_saturation = true;
            m.vsatn = number(mf->text(QStringLiteral("vsatn")), velocity, where);
            m.vsatp = number(mf->text(QStringLiteral("vsatp")), velocity, where);
            m.wholeField = mf->text(QStringLiteral("driving")) == QLatin1String("field");
            a_alongCurrent = a_alongCurrent || !m.wholeField;
        }
        if (mf && mf->flag(QStringLiteral("surface"))) {
            m.surface = a_lombardi = true;
            const Dim perSecond = Dim::voltage() / Dim::of(0, 0, 1);
            m.sBn = number(mf->text(QStringLiteral("Bn")), velocity, where);
            m.sCn = number(mf->text(QStringLiteral("Cn")), Dim(), where);
            m.sLn = number(mf->text(QStringLiteral("lambdan")), Dim(), where);
            m.sDn = number(mf->text(QStringLiteral("deltan")), perSecond, where);
            m.sBp = number(mf->text(QStringLiteral("Bp")), velocity, where);
            m.sCp = number(mf->text(QStringLiteral("Cp")), Dim(), where);
            m.sLp = number(mf->text(QStringLiteral("lambdap")), Dim(), where);
            m.sDp = number(mf->text(QStringLiteral("deltap")), perSecond, where);
            m.lcrit = number(mf->text(QStringLiteral("lcrit")), Dim::length(), where);
            if (!(m.sBn > 0) || !(m.sCn > 0) || !(m.sDn > 0) || !(m.sBp > 0) || !(m.sCp > 0) || !(m.sDp > 0) || !(m.lcrit > 0))
                problem(tr("%1: its surface scattering's B, C, δ and lcrit must be more than 0").arg(where));
        }
        if (mf && mf->flag(QStringLiteral("bgn"))) {
            m.bgn = true;
            m.bgnE = energyOf(mf->text(QStringLiteral("Ebgn")), constants, &why);
            if (!std::isfinite(m.bgnE)) problem(tr("%1: its band gap narrowing's energy: %2").arg(where, why));
            m.bgnN = number(mf->text(QStringLiteral("Nbgn")), conc, where);
            m.bgnC = number(mf->text(QStringLiteral("Cbgn")), Dim(), where);
            if (!(m.bgnN > 0) || !(m.bgnC >= 0)) problem(tr("%1: its band gap narrowing's Nref must be more than 0, C no less").arg(where));
        }
        if (mf && mf->flag(QStringLiteral("fermi"))) m.fermi = a_fermi = true;
        if (mf && mf->flag(QStringLiteral("incomplete"))) {
            m.incomplete = a_incomplete = true;
            const double Ed = energyOf(mf->text(QStringLiteral("Ed")), constants, &why);
            const double Ea = energyOf(mf->text(QStringLiteral("Ea")), constants, &why);
            m.gd = number(mf->text(QStringLiteral("gd")), Dim(), where);
            m.ga = number(mf->text(QStringLiteral("ga")), Dim(), where);
            if (!std::isfinite(Ed) || !std::isfinite(Ea) || !(m.gd > 0) || !(m.ga > 0))
                problem(tr("%1: its dopants' levels and degeneracies (more than 0)").arg(where));
            m.n1d = m.Nc * std::exp(-Ed / (KB * a_T));
            m.p1a = m.Nv * std::exp(-Ea / (KB * a_T));
        }
        if (const Node* r = srhOf[std::size_t(d)]) {
            m.srh = true;
            const bool own = r->text(QStringLiteral("source")) == QLatin1String("user");
            auto rt = [&](const QString& key) { return own ? r->text(key) : mat ? materialProperty(*mat, key, &constants) : QString(); };
            const QString tn = rt(QStringLiteral("taun")), tp = rt(QStringLiteral("taup"));
            if (tn.trimmed().isEmpty() || tp.trimmed().isEmpty()) problem(tr("%1: its material has no carrier lifetimes (taun, taup)").arg(where));
            else {
                m.taun = number(tn, Dim::of(0, 0, 1), where);
                m.taup = number(tp, Dim::of(0, 0, 1), where);
            }
            m.Et = energyOf(r->text(QStringLiteral("Et")), constants, &why);
            if (!std::isfinite(m.Et)) m.Et = 0;
        }
        if (const Node* a = augerOf[std::size_t(d)]) {
            m.Cn = number(a->text(QStringLiteral("Cn")), Dim::of(6, 0, -1), where);
            m.Cp = number(a->text(QStringLiteral("Cp")), Dim::of(6, 0, -1), where);
        }
        if (const Node* r = radiativeOf[std::size_t(d)]) m.B = number(r->text(QStringLiteral("B")), Dim::of(3, 0, -1), where);
        if (const Node* ii = impactOf[std::size_t(d)]) {
            m.ionization = a_ionizationAny = true;
            m.ionizationCurrents = ii->flag(QStringLiteral("currents"));
            a_ionization = a_ionization || m.ionizationCurrents;
            if (ii->text(QStringLiteral("model")) == QLatin1String("user")) {
                const Dim perMetre = Dim::of(-1), field = Dim::of(1, 1, -3, -1);
                m.an = number(ii->text(QStringLiteral("an")), perMetre, ii->name());
                m.bn = number(ii->text(QStringLiteral("bn")), field, ii->name());
                m.an2 = number(ii->text(QStringLiteral("an2")), perMetre, ii->name());
                m.bn2 = number(ii->text(QStringLiteral("bn2")), field, ii->name());
                m.ap = number(ii->text(QStringLiteral("ap")), perMetre, ii->name());
                m.bp = number(ii->text(QStringLiteral("bp")), field, ii->name());
                m.ap2 = number(ii->text(QStringLiteral("ap2")), perMetre, ii->name());
                m.bp2 = number(ii->text(QStringLiteral("bp2")), field, ii->name());
                m.E0 = number(ii->text(QStringLiteral("E0")), field, ii->name());
                m.ionBetaN = number(ii->text(QStringLiteral("betan")), Dim(), ii->name());
                m.ionBetaP = number(ii->text(QStringLiteral("betap")), Dim(), ii->name());
            } else {
                // Silicon at 300 K (van Overstraeten and de Man, 1970).
                m.an = m.an2 = 7.03e7;
                m.bn = m.bn2 = 1.231e8;
                m.ap = 1.582e8;
                m.bp = 2.036e8;
                m.ap2 = 6.71e7;
                m.bp2 = 1.693e8;
                m.E0 = 4e7;
                m.ionBetaN = m.ionBetaP = 1;
            }
        }
    }
    if (!referenced && !domains.isEmpty()) problem(tr("%1: none of its domains is a semiconductor (a material with a band gap Eg)").arg(a_node->name()));
    // Elements and nodes.
    const int nt = sp.elements();
    a_elemSemi.assign(std::size_t(nt), 0);
    a_elemOn.assign(std::size_t(nt), 0);
    a_nodeDomain.assign(std::size_t(a_n), -1);
    std::vector<char> nodeOn(std::size_t(a_n), 0);
    a_box.assign(std::size_t(a_n), 0);
    a_K.assign(std::size_t(nt), {});
    std::map<std::pair<int, int>, int> edgeIndex;
    a_edges.clear();
    const double scale = mesh.unitScale;
    for (int t = 0; t < nt; ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        a_elemOn[std::size_t(t)] = 1;
        const Medium& m = a_media[std::size_t(d)];
        const int* dof = sp.dofs(t);
        QPointF x[3];
        for (int k = 0; k < 3; ++k) {
            x[k] = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * scale;
            nodeOn[std::size_t(dof[k])] = 1;
        }
        const double det = (x[1].x() - x[0].x()) * (x[2].y() - x[0].y()) - (x[2].x() - x[0].x()) * (x[1].y() - x[0].y());
        const double area = std::abs(det) / 2;
        const double weight = a_s.weight((x[0] + x[1] + x[2]) / 3.0);
        // ∇φk = perpendicular of the opposite edge over 2 A (signed).
        double gx[3], gy[3];
        for (int k = 0; k < 3; ++k) {
            const QPointF& a = x[(k + 1) % 3];
            const QPointF& b = x[(k + 2) % 3];
            gx[k] = (a.y() - b.y()) / det;
            gy[k] = (b.x() - a.x()) / det;
        }
        std::array<double, 9>& K = a_K[std::size_t(t)];
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) K[std::size_t(3 * a + b)] = Eps0 * m.epsr * area * weight * (gx[a] * gx[b] + gy[a] * gy[b]);
        if (!m.semiconductor) continue;
        a_elemSemi[std::size_t(t)] = 1;
        for (int k = 0; k < 3; ++k) {
            a_box[std::size_t(dof[k])] += area * weight / 3;
            if (a_nodeDomain[std::size_t(dof[k])] < 0) a_nodeDomain[std::size_t(dof[k])] = d;
        }
        for (int k = 0; k < 3; ++k) {
            const int a = dof[k], b = dof[(k + 1) % 3];
            const auto key = std::minmax(a, b);
            auto it = edgeIndex.find({key.first, key.second});
            if (it == edgeIndex.end()) {
                it = edgeIndex.emplace(std::pair{key.first, key.second}, int(a_edges.size())).first;
                Edge e;
                e.i = key.first;
                e.j = key.second;
                e.h = std::hypot(x[(k + 1) % 3].x() - x[k].x(), x[(k + 1) % 3].y() - x[k].y());
                const QPointF along = (x[(k + 1) % 3] - x[k]) / e.h * (a == e.i ? 1.0 : -1.0);
                e.tx = along.x();
                e.ty = along.y();
                a_edges.push_back(e);
            }
            // (Its weights: edgeWeights(), once the mobilities are known.)
        }
    }
    // Doping, by node: each feature's in its share of the node's box (a node
    // on a junction between two domains doped differently takes their mean).
    a_Nd.assign(std::size_t(a_n), 0);
    a_Na.assign(std::size_t(a_n), 0);
    a_nodePoint.assign(std::size_t(a_n), {-1, {0, 0, 0}});
    struct Share {
        int domain, triangle, corner;
        double area;
    };
    std::vector<std::vector<Share>> shares(static_cast<std::size_t>(a_n));
    for (int t = 0; t < nt; ++t) {
        if (!a_elemOn[std::size_t(t)]) continue;
        const int* dof = sp.dofs(t);
        static const double ref[3][2] = {{0, 0}, {1, 0}, {0, 1}};
        for (int k = 0; k < 3; ++k) {
            const int i = dof[k];
            if (a_nodePoint[std::size_t(i)].first < 0 || (a_elemSemi[std::size_t(t)] && !a_elemSemi[std::size_t(a_nodePoint[std::size_t(i)].first)]))
                a_nodePoint[std::size_t(i)] = {t, {ref[k][0], ref[k][1], 0}};
            if (!a_elemSemi[std::size_t(t)]) continue;
            const QPointF a = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][0])], b = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][1])],
                          c = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][2])];
            const double area = std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y()));
            shares[std::size_t(i)].push_back({mesh.domain[std::size_t(t)], t, k, area});
        }
    }
    PointEvaluator ev(a_s);
    Expression::Options inUnit;
    inUnit.lengthUnit = topo.unitScale;
    inUnit.numbersAreLengths = true;
    for (const DopingDef& dd : dopings) {
        const Node& df = *dd.node;
        const bool donor = df.text(QStringLiteral("dopant")) == QLatin1String("donor");
        const QString profile = df.text(QStringLiteral("profile"));
        std::vector<char> on(topo.domains.size(), 0);
        for (int d : dd.domains) on[std::size_t(d)] = 1;
        Expression N;
        double N0 = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, sigma = 0;
        if (profile == QLatin1String("expression")) N = compile(df.text(QStringLiteral("N")), df, QStringLiteral("N"));
        else {
            N0 = number(df.text(QStringLiteral("N0")), conc, df.name());
            if (profile == QLatin1String("box")) {
                auto length = [&](const QJsonValue& v) {
                    const Expression e = Expression::compile(v.toVariant().toString(), constants, inUnit);
                    if (!e.isValid() || !e.isConstant()) {
                        problem(tr("%1: %2").arg(df.name(), e.isValid() ? tr("its box must be constants") : e.error()));
                        return 0.0;
                    }
                    return e.constant() * topo.unitScale;
                };
                const QJsonArray base = df.value(QStringLiteral("position")).toArray(), size = df.value(QStringLiteral("size")).toArray();
                x0 = length(base.at(0));
                y0 = length(base.at(1));
                x1 = x0 + length(size.at(0));
                y1 = y0 + length(size.at(1));
                if (x1 < x0) std::swap(x0, x1);
                if (y1 < y0) std::swap(y0, y1);
                const double dj = number(df.text(QStringLiteral("dj")), Dim::length(), df.name());
                const double Nb = number(df.text(QStringLiteral("Nb")), conc, df.name());
                if (!(dj > 0) || !(Nb > 0) || !(N0 > Nb)) problem(tr("%1: its junction depth, background and peak must be more than 0, the peak more than the background").arg(df.name()));
                else sigma = dj / std::sqrt(std::log(N0 / Nb));
            }
        }
        static const double ref[3][2] = {{0, 0}, {1, 0}, {0, 1}};
        for (int i = 0; i < a_n; ++i) {
            double inside = 0, all = 0;
            const Share* where = nullptr;
            for (const Share& sh : shares[std::size_t(i)]) {
                all += sh.area;
                if (on[std::size_t(sh.domain)]) {
                    inside += sh.area;
                    where = &sh;
                }
            }
            if (!where || !(all > 0)) continue;
            const double frac = inside / all;
            double v = 0;
            if (profile == QLatin1String("expression")) {
                if (!N.isValid()) continue;
                ev.moveTo(where->triangle, ref[where->corner][0], ref[where->corner][1]);
                v = ev.eval(N);
            } else if (profile == QLatin1String("box")) {
                const QPointF p = sp.point(i);
                const double dx = std::max({x0 - p.x(), 0.0, p.x() - x1}), dy = std::max({y0 - p.y(), 0.0, p.y() - y1});
                const double dist = std::hypot(dx, dy);
                v = sigma > 0 ? N0 * std::exp(-(dist / sigma) * (dist / sigma)) : (dist == 0 ? N0 : 0);
            } else {
                v = N0;
            }
            if (!std::isfinite(v) || v < 0) continue;
            (donor ? a_Nd : a_Na)[std::size_t(i)] += frac * v;
        }
    }
    a_ni.assign(std::size_t(a_n), 0);
    a_n1.assign(std::size_t(a_n), 0);
    a_p1.assign(std::size_t(a_n), 0);
    a_delta.assign(std::size_t(a_n), 0);
    a_thetaN.assign(std::size_t(a_n), 0);
    a_thetaP.assign(std::size_t(a_n), 0);
    a_scale = 0;
    a_gap.assign(std::size_t(a_n), 0);
    for (auto* v : {&a_lgN, &a_lgP, &a_dlgN, &a_dlgP}) v->assign(std::size_t(a_n), 0.0);
    a_rN.assign(std::size_t(a_n), 1.0);
    a_rP.assign(std::size_t(a_n), 1.0);
    bool hetero = false;
    for (int i = 0; i < a_n; ++i) {
        const int d = a_nodeDomain[std::size_t(i)];
        if (d < 0) continue;
        const Medium& m = a_media[std::size_t(d)];
        double gap = 0;
        if (m.bgn) {
            const double N = a_Nd[std::size_t(i)] + a_Na[std::size_t(i)];
            if (N > 0) {
                const double l = std::log(N / m.bgnN);
                gap = m.bgnE * (l + std::sqrt(l * l + m.bgnC));
            }
        }
        a_gap[std::size_t(i)] = gap;
        const double ni = m.ni * std::exp(gap / (2 * KB * a_T));
        a_ni[std::size_t(i)] = ni;
        a_n1[std::size_t(i)] = ni * std::exp(m.Et / (KB * a_T));
        a_p1[std::size_t(i)] = ni * std::exp(-m.Et / (KB * a_T));
        a_scale = std::max({a_scale, std::abs(doping(i)), ni});
        // (A node on a heterojunction is its first semiconductor's: the
        // bands' step is taken along the edges into the other. The gap's
        // narrowing a step of its own: its bands' edges, e^(ΔEg/2kT) in ni.)
        const double delta = (m.chi + m.dEc - a_refChi - a_refDEc) / Q, spread = a_Vt * std::log(ni / a_refNi);
        a_delta[std::size_t(i)] = delta;
        a_thetaN[std::size_t(i)] = delta + spread;
        a_thetaP[std::size_t(i)] = delta - spread;
        hetero = hetero || std::abs(delta) > 1e-9 || std::abs(m.ni / a_refNi - 1) > 1e-9;
    }
    // Schrödinger-Poisson's nodes, their quantum potentials nought to start.
    a_thetaN0 = a_thetaN;
    a_thetaP0 = a_thetaP;
    a_lamN.assign(std::size_t(a_n), 0.0);
    a_lamP.assign(std::size_t(a_n), 0.0);
    a_quantumNodes.clear();
    if (a_quantumElectrons || a_quantumHoles)
        for (int t = 0; t < nt; ++t) {
            if (!a_elemSemi[std::size_t(t)] || !quantumOn[std::size_t(mesh.domain[std::size_t(t)])]) continue;
            for (int k = 0; k < 3; ++k) a_quantumNodes.push_back(sp.dofs(t)[k]);
        }
    std::sort(a_quantumNodes.begin(), a_quantumNodes.end());
    a_quantumNodes.erase(std::unique(a_quantumNodes.begin(), a_quantumNodes.end()), a_quantumNodes.end());
    if (hetero) {
        const QString line = tr("%1: heterojunctions - the bands' offsets (Anderson's rule: the electron affinities, the band gaps) "
                                "between its semiconductors")
                                 .arg(a_node->name());
        if (!a_s.log.contains(line)) a_s.log << line;
    }
    if (!(a_scale > 0)) a_scale = 1e16;
    // Mobilities by element (doping: the element's nodes' mean), then by edge.
    std::vector<double> mun(std::size_t(nt), 0), mup(std::size_t(nt), 0);
    for (int t = 0; t < nt; ++t) {
        if (!a_elemSemi[std::size_t(t)]) continue;
        const Medium& m = a_media[std::size_t(mesh.domain[std::size_t(t)])];
        const int* dof = sp.dofs(t);
        double N = 0;
        for (int k = 0; k < 3; ++k) N += (a_Nd[std::size_t(dof[k])] + a_Na[std::size_t(dof[k])]) / 3;
        mun[std::size_t(t)] = m.mun;
        mup[std::size_t(t)] = m.mup;
        if (m.dopingMobility) {
            mun[std::size_t(t)] = m.munMin + (m.mun - m.munMin) / (1 + std::pow(N / m.nRefN, m.alphaN));
            mup[std::size_t(t)] = m.mupMin + (m.mup - m.mupMin) / (1 + std::pow(N / m.nRefP, m.alphaP));
        }
    }
    a_elements.assign(std::size_t(nt), ElementMobility());
    for (int t = 0; t < nt; ++t) {
        if (!a_elemSemi[std::size_t(t)]) continue;
        ElementMobility& em = a_elements[std::size_t(t)];
        em.mun = mun[std::size_t(t)];
        em.mup = mup[std::size_t(t)];
        em.domain = mesh.domain[std::size_t(t)];
        const int* dof = sp.dofs(t);
        for (int k = 0; k < 3; ++k) em.N += (a_Nd[std::size_t(dof[k])] + a_Na[std::size_t(dof[k])]) / 3;
        QPointF x[3];
        for (int k = 0; k < 3; ++k) x[k] = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * scale;
        const double weight = a_s.weight((x[0] + x[1] + x[2]) / 3.0);
        const double det = (x[1].x() - x[0].x()) * (x[2].y() - x[0].y()) - (x[2].x() - x[0].x()) * (x[1].y() - x[0].y());
        em.volume = std::abs(det) / 2 * weight;
        for (int k = 0; k < 3; ++k) {
            const auto key = std::minmax(dof[k], dof[(k + 1) % 3]);
            em.edge[k] = edgeIndex.at({key.first, key.second});
            Edge& edge = a_edges[std::size_t(em.edge[k])];
            if (edge.domain < 0 || (!a_media[std::size_t(edge.domain)].ionization && a_media[std::size_t(em.domain)].ionization)) edge.domain = em.domain;
            // The edge's cot weight from this triangle: half the cotangent of
            // the angle opposite it.
            const QPointF u = x[k] - x[(k + 2) % 3], v = x[(k + 1) % 3] - x[(k + 2) % 3];
            const double cot = (u.x() * v.x() + u.y() * v.y()) / std::abs(u.x() * v.y() - u.y() * v.x());
            em.cot[k] = 0.5 * cot * weight;
            const QPointF& a = x[(k + 1) % 3];
            const QPointF& b = x[(k + 2) % 3];
            em.gx[k] = (a.y() - b.y()) / det;
            em.gy[k] = (b.x() - a.x()) / det;
        }
    }
    // Each triangle's neighbours (a field line followed across them).
    a_neighbors.assign(std::size_t(nt), {-1, -1, -1});
    {
        std::map<std::pair<int, int>, std::pair<int, int>> side;   // an edge: (triangle, its edge k)
        for (int t = 0; t < nt; ++t)
            for (int k = 0; k < 3; ++k) {
                const int a = mesh.triangles[std::size_t(t)][std::size_t(k)], b = mesh.triangles[std::size_t(t)][std::size_t((k + 1) % 3)];
                const auto key = std::minmax(a, b);
                const auto it = side.find({key.first, key.second});
                if (it == side.end()) side.emplace(std::pair{key.first, key.second}, std::pair{t, k});
                else {
                    a_neighbors[std::size_t(t)][std::size_t(k)] = it->second.first;
                    a_neighbors[std::size_t(it->second.first)][std::size_t(it->second.second)] = t;
                }
            }
    }
    // Surface scattering: each triangle's nearest interface with an
    // insulator (a side shared with one), its normal and the distance to it
    // from the triangle's centre.
    if (a_lombardi) {
        struct Side {
            QPointF a, b;
            double nx, ny;
        };
        std::vector<Side> sides;
        for (int t = 0; t < nt; ++t) {
            if (!a_elemSemi[std::size_t(t)]) continue;
            for (int k = 0; k < 3; ++k) {
                const int u = a_neighbors[std::size_t(t)][std::size_t(k)];
                if (u < 0 || !a_elemOn[std::size_t(u)] || a_elemSemi[std::size_t(u)]) continue;
                const QPointF a = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * scale;
                const QPointF b = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t((k + 1) % 3)])] * scale;
                const double h = std::hypot(b.x() - a.x(), b.y() - a.y());
                if (h > 0) sides.push_back({a, b, -(b.y() - a.y()) / h, (b.x() - a.x()) / h});
            }
        }
        if (sides.empty()) {
            const QString line = tr("%1: surface scattering, but no semiconductor meets an insulator: none").arg(a_node->name());
            if (!a_s.log.contains(line)) a_s.log << line;
        }
        for (int t = 0; t < nt && !sides.empty(); ++t) {
            ElementMobility& em = a_elements[std::size_t(t)];
            if (em.domain < 0 || !a_media[std::size_t(em.domain)].surface) continue;
            QPointF c;
            for (int k = 0; k < 3; ++k) c += mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * scale / 3.0;
            double best = Inf;
            const Side* nearest = nullptr;
            for (const Side& side : sides) {
                const double ux = side.b.x() - side.a.x(), uy = side.b.y() - side.a.y();
                const double f = std::clamp(((c.x() - side.a.x()) * ux + (c.y() - side.a.y()) * uy) / (ux * ux + uy * uy), 0.0, 1.0);
                const double d = std::hypot(c.x() - side.a.x() - f * ux, c.y() - side.a.y() - f * uy);
                if (d < best) {
                    best = d;
                    nearest = &side;
                }
            }
            em.nx = nearest->nx;
            em.ny = nearest->ny;
            em.damp = std::exp(-best / a_media[std::size_t(em.domain)].lcrit);
        }
    }
    edgeWeights(nullptr);
    // Contacts and surface charges.
    a_contacts.clear();
    a_nodeContact.assign(std::size_t(a_n), -1);
    a_surface.clear();
    a_surfaceLength.clear();
    a_traps.clear();
    for (const Node* bf : boundaryFeatures) {
        const QString t = bf->type;
        const QVector<int> picked = resolveSelection(topo, model, Level::Boundary, bf->selection(), &why);
        std::vector<char> on(topo.boundaries.size(), 0);
        for (int b : picked) on[std::size_t(b)] = 1;
        // Its nodes, and each one's share of its length (m², weighted).
        std::map<int, double> share;
        for (const Mesh::Edge& me : mesh.edges) {
            if (me.boundary < 0 || !on[std::size_t(me.boundary)]) continue;
            const QPointF a = mesh.nodes[std::size_t(me.a)] * scale, b = mesh.nodes[std::size_t(me.b)] * scale;
            const double len = std::hypot(b.x() - a.x(), b.y() - a.y()) * a_s.weight((a + b) / 2.0);
            if (!nodeOn[std::size_t(me.a)] && !nodeOn[std::size_t(me.b)]) continue;
            share[me.a] += len / 2;
            share[me.b] += len / 2;
        }
        if (t == QLatin1String("semiconductor/charge")) {
            const Expression e = compile(bf->text(QStringLiteral("rhos")), *bf, QStringLiteral("rhos"));
            if (!e.isValid()) continue;
            for (const auto& [node, len] : share) {
                a_surface.emplace_back(node, e);
                a_surfaceLength.push_back(len);
            }
            continue;
        }
        if (t == QLatin1String("semiconductor/traps")) {
            const QString where = bf->name();
            const double sn = number(bf->text(QStringLiteral("sigman")), Dim::area(), where);
            const double sp = number(bf->text(QStringLiteral("sigmap")), Dim::area(), where);
            const double vth = number(bf->text(QStringLiteral("vth")), velocity, where);
            if (!(sn > 0) || !(sp > 0) || !(vth > 0)) {
                problem(tr("%1: its cross sections and thermal velocity must be more than 0").arg(where));
                continue;
            }
            // Its levels: one, or the range's in equal steps (each Dit ΔE at
            // its middle).
            std::vector<std::pair<double, double>> levels;   // (Et - Ei in J, 1/m²)
            if (bf->text(QStringLiteral("distribution")) == QLatin1String("level")) {
                const double Nit = number(bf->text(QStringLiteral("Nit")), Dim::of(-2), where);
                const double Et = energyOf(bf->text(QStringLiteral("Et")), constants, &why);
                if (!(Nit >= 0) || !std::isfinite(Et)) problem(tr("%1: its density and level").arg(where));
                else levels.emplace_back(Et, Nit);
            } else {
                const double Dit = number(bf->text(QStringLiteral("Dit")), Dim::of(-4, -1, 2), where);
                const double lo = energyOf(bf->text(QStringLiteral("Elow")), constants, &why);
                const double hi = energyOf(bf->text(QStringLiteral("Ehigh")), constants, &why);
                const int count = std::max(1, bf->integer(QStringLiteral("levels")));
                if (!(Dit >= 0) || !std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo)) problem(tr("%1: its density and range").arg(where));
                else
                    for (int k = 0; k < count; ++k) levels.emplace_back(lo + (k + 0.5) * (hi - lo) / count, Dit * (hi - lo) / count);
            }
            const bool acceptor = bf->text(QStringLiteral("kind")) != QLatin1String("donor");
            for (const auto& [node, len] : share) {
                if (a_nodeDomain[std::size_t(node)] < 0) continue;
                const double ni = a_ni[std::size_t(node)];
                for (const auto& [Et, N] : levels) {
                    TrapLevel tl;
                    tl.node = node;
                    tl.area = len;
                    tl.N = N;
                    tl.n1 = ni * std::exp(Et / (KB * a_T));
                    tl.p1 = ni * std::exp(-Et / (KB * a_T));
                    tl.cn = sn * vth;
                    tl.cp = sp * vth;
                    tl.acceptor = acceptor;
                    a_traps.push_back(tl);
                }
            }
            continue;
        }
        if (t != QLatin1String("semiconductor/ohmic") && t != QLatin1String("semiconductor/schottky") && t != QLatin1String("semiconductor/gate"))
            continue;
        Contact c;
        c.name = bf->text(QStringLiteral("name"));
        c.kind = t == QLatin1String("semiconductor/ohmic") ? Contact::Ohmic : t == QLatin1String("semiconductor/gate") ? Contact::Gate : Contact::Schottky;
        c.V0 = compile(bf->text(QStringLiteral("V0")), *bf, QStringLiteral("V0"), false);
        if (c.kind == Contact::Schottky) {
            c.phiB = energyOf(bf->text(QStringLiteral("phiB")), constants, &why);
            if (!std::isfinite(c.phiB)) problem(tr("%1: its barrier: %2").arg(bf->name(), why));
            c.vn = number(bf->text(QStringLiteral("vn")), velocity, bf->name());
            c.vp = number(bf->text(QStringLiteral("vp")), velocity, bf->name());
        }
        if (c.kind == Contact::Gate) {
            c.phim = energyOf(bf->text(QStringLiteral("phim")), constants, &why);
            if (!std::isfinite(c.phim)) problem(tr("%1: its work function: %2").arg(bf->name(), why));
        }
        for (const Contact& other : a_contacts)
            if (other.name == c.name) problem(tr("two contacts are named %1").arg(c.name));
        const int index = int(a_contacts.size());
        for (const auto& [node, len] : share) {
            if (a_nodeContact[std::size_t(node)] >= 0) continue;   // (a corner: the first contact's)
            if (c.kind == Contact::Gate && a_nodeDomain[std::size_t(node)] >= 0 && c.nodes.empty())
                a_s.log << tr("Warning: %1 touches a semiconductor: a gate is on an insulator (a metal on a semiconductor is a Schottky "
                              "or ohmic contact); its potential is fixed there")
                               .arg(bf->name());
            if ((c.kind == Contact::Ohmic || c.kind == Contact::Schottky) && a_nodeDomain[std::size_t(node)] < 0) continue;
            a_nodeContact[std::size_t(node)] = index;
            c.nodes.push_back(node);
            c.length.push_back(len);
        }
        if (c.nodes.empty()) problem(tr("%1 is on no boundary of %2's %3").arg(bf->name(), a_node->name(),
                                                                                c.kind == Contact::Gate ? tr("domains") : tr("semiconductors")));
        a_contacts.push_back(c);
    }
    bool carriersHeld = false;
    for (const Contact& c : a_contacts) carriersHeld = carriersHeld || c.kind != Contact::Gate;
    if (!carriersHeld && referenced) problem(tr("%1 has no metal contact on a semiconductor (ohmic or Schottky): its carriers' level is not set").arg(a_node->name()));
    // The unknowns: V where the physics is, n and p on its semiconductors.
    a_iV.assign(std::size_t(a_n), -1);
    a_iN.assign(std::size_t(a_n), -1);
    a_iP.assign(std::size_t(a_n), -1);
    a_unknowns = 0;
    for (int i = 0; i < a_n; ++i)
        if (nodeOn[std::size_t(i)]) a_iV[std::size_t(i)] = a_unknowns++;
    for (int i = 0; i < a_n; ++i)
        if (a_nodeDomain[std::size_t(i)] >= 0) {
            a_iN[std::size_t(i)] = a_unknowns++;
            a_iP[std::size_t(i)] = a_unknowns++;
        }
    a_bcV.assign(std::size_t(a_n), 0);
    a_bcN.assign(std::size_t(a_n), 0);
    a_bcP.assign(std::size_t(a_n), 0);
    // The field's constant parts: the doping; what the variables read.
    f.values.resize(std::size_t(7) * std::size_t(a_n), 0.0);
    for (int i = 0; i < a_n; ++i) {
        f.values[std::size_t(3 * a_n + i)] = a_Nd[std::size_t(i)];
        f.values[std::size_t(4 * a_n + i)] = a_Na[std::size_t(i)];
        f.values[std::size_t(5 * a_n + i)] = a_thetaN[std::size_t(i)];
        f.values[std::size_t(6 * a_n + i)] = a_thetaP[std::size_t(i)];
    }
    // (The layout: ExtraHead's.)
    f.extra.assign(std::size_t(ExtraHead + PerTriangle * nt), NaN);
    f.extra[0] = a_Vt;
    f.extra[1] = a_T;
    f.extra[2] = a_refNi;
    for (int t = 0; t < nt; ++t) {
        if (!a_elemSemi[std::size_t(t)]) continue;
        const Medium& m = a_media[std::size_t(mesh.domain[std::size_t(t)])];
        double* e = &f.extra[std::size_t(ExtraHead + PerTriangle * t)];
        // (The gap's narrowing, its corners' mean: the bands closed by it.)
        const int* tdof = sp.dofs(t);
        const double gap = (a_gap[std::size_t(tdof[0])] + a_gap[std::size_t(tdof[1])] + a_gap[std::size_t(tdof[2])]) / 3;
        e[0] = mun[std::size_t(t)];
        e[1] = mup[std::size_t(t)];
        e[2] = m.dEc - gap / 2;
        e[3] = m.Eg - gap;
        e[4] = m.ni * std::exp(gap / (2 * KB * a_T));
        e[5] = m.srh ? m.taun : Inf;
        e[6] = m.srh ? m.taup : Inf;
        e[7] = m.Et;
        e[8] = m.Cn;
        e[9] = m.Cp;
        e[10] = m.B;
        e[11] = (m.chi + m.dEc - a_refChi - a_refDEc) / Q;
        e[12] = m.incomplete ? m.n1d : Inf;
        e[13] = m.gd;
        e[14] = m.incomplete ? m.p1a : Inf;
        e[15] = m.ga;
        e[16] = m.fermi ? m.Nc : 0.0;
        e[17] = m.fermi ? m.Nv : 0.0;
        e[18] = 0;
    }
    Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    coef.c.assign(topo.domains.size(), Expression());
    for (int d : domains) coef.c[std::size_t(d)] = Expression::compile(QString::number(a_media[std::size_t(d)].epsr, 'g', 17), a_s.pointScope());
    // Contacts as the field's terminals (their applied voltages kept with it).
    if (f.terminals.size() != a_contacts.size()) {
        f.terminals.clear();
        for (const Contact& c : a_contacts) {
            Field::Terminal term;
            term.name = c.name;
            f.terminals.push_back(term);
        }
    }
    // (Kept with them: the voltage applied, the current, the metal's charge.)
    for (std::size_t k = 0; k < a_contacts.size(); ++k)
        if (f.terminals[k].name == a_contacts[k].name) {
            a_contacts[k].applied = f.terminals[k].potential;
            a_contacts[k].current = f.terminals[k].charge;
            a_contacts[k].charge = f.terminals[k].chargeIm;
        }
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    if (a_clamped > 0) {
        const QString line = tr("%1: %2 of %3 edges not Delaunay (their currents left out)").arg(a_node->name()).arg(a_clamped).arg(a_edges.size());
        if (!a_s.log.contains(line)) a_s.log << line;
    }
    return true;
}

void SemiconductorProblem::neutral(int i, double* n, double* p, double* v) const
{
    const double C = doping(i), ni = a_ni[std::size_t(i)];
    const double half = std::abs(C) / 2;
    const double major = half + std::sqrt(half * half + ni * ni);
    const double minor = ni * ni / major;
    *n = C >= 0 ? major : minor;
    *p = C >= 0 ? minor : major;
    if (v) *v = std::log(*n / ni);
    const int d = a_nodeDomain[std::size_t(i)];
    if (d < 0 || (!a_media[std::size_t(d)].incomplete && !a_media[std::size_t(d)].fermi)) return;
    // Fermi-Dirac's carriers, or the dopants ionized in part: p - n + Nd⁺ -
    // Na⁻ = 0, falling as the potential v rises - halved to a double's
    // digits. (The dopants' occupation by the Fermi level: by Boltzmann's
    // ni e^v, whatever the carriers' statistics.)
    auto charge = [&](double u) {
        double nu = 0, pu = 0, dn = 0, dp = 0, nd = 0, dnd = 0, na = 0, dna = 0;
        carriersAt(i, u, &nu, &pu, &dn, &dp);
        ionized(i, ni * std::exp(std::clamp(u, -600.0, 600.0)), ni * std::exp(std::clamp(-u, -600.0, 600.0)), &nd, &dnd, &na, &dna);
        return pu - nu + nd - na;
    };
    const double v0 = std::asinh(C / (2 * ni));
    double lo = v0 - 1, hi = v0 + 1;
    while (charge(lo) < 0 && lo > -600) lo -= 10;
    while (charge(hi) > 0 && hi < 600) hi += 10;
    for (int k = 0; k < 200 && hi - lo > 1e-15 * std::max(1.0, std::abs(lo)); ++k) {
        const double mid = (lo + hi) / 2;
        (charge(mid) > 0 ? lo : hi) = mid;
    }
    const double u = (lo + hi) / 2;
    double dn = 0, dp = 0;
    carriersAt(i, u, n, p, &dn, &dp);
    if (v) *v = u;
}

void SemiconductorProblem::carriersAt(int i, double v, double* n, double* p, double* dn, double* dp) const
{
    const double ni = a_ni[std::size_t(i)];
    v = std::clamp(v, -600.0, 600.0);
    // (Schrödinger-Poisson's quantum potentials: each carrier e^(Λ/Vt).)
    *n = ni * std::exp(std::clamp(v + a_lamN[std::size_t(i)] / a_Vt, -600.0, 600.0));
    *p = ni * std::exp(std::clamp(-v + a_lamP[std::size_t(i)] / a_Vt, -600.0, 600.0));
    *dn = *n;
    *dp = -*p;
    const int d = a_nodeDomain[std::size_t(i)];
    if (d < 0 || !a_media[std::size_t(d)].fermi) return;
    // n = Nc ℱ(η), Boltzmann's Nc e^η = ni e^v.
    const Medium& m = a_media[std::size_t(d)];
    const FermiHalf& fh = fermiHalf();
    double F = 0, dF = 0;
    fh.at(std::log(*n / m.Nc), &F, &dF);
    *n = m.Nc * F;
    *dn = m.Nc * dF;
    fh.at(std::log(*p / m.Nv), &F, &dF);
    *p = m.Nv * F;
    *dp = -m.Nv * dF;
}

double SemiconductorProblem::quantumCorrection(QString* error)
{
    (void)error;
    if (a_quantumNodes.empty()) return 0;
    // Each node's quantum potential moved by Vt ln(n_states/n): the
    // semiconductor's carriers there the states' next time - its carriers
    // e^(Λ/Vt) Boltzmann's, so Poisson's equation solved with them takes the
    // states' as rising e^(δV/Vt) with the potential (Trellakis's predictor
    // and corrector). (A step no more than 3 Vt, Λ within ±30 Vt; where the
    // states have almost none - ψ nought at an oxide - floored at 1e-8 of
    // their most, not counted below 1e-6 of it.) Once the steps are under
    // Vt, Anderson's mixing of the last five: the plain steps crawled at 0.96
    // a turn, the states' shape answering the potential's, not each node's.
    const Field& f = field();
    const std::size_t N = std::size_t(a_n);
    PointEvaluator ev(a_s);
    std::vector<double> nq(a_quantumNodes.size(), NaN), pq(a_quantumNodes.size(), NaN);
    double mostN = 0, mostP = 0;
    for (std::size_t k = 0; k < a_quantumNodes.size(); ++k) {
        const auto& [t, r] = a_nodePoint[std::size_t(a_quantumNodes[k])];
        ev.moveTo(t, r[0], r[1]);
        if (a_quantumElectrons && a_quantumN.isValid()) mostN = std::max(mostN, nq[k] = ev.eval(a_quantumN));
        if (a_quantumHoles && a_quantumP.isValid()) mostP = std::max(mostP, pq[k] = ev.eval(a_quantumP));
    }
    // The entries: each quantum node's Λn and Λp, their steps, and whether
    // their carriers count.
    std::vector<double*> lam;
    std::vector<double> step;
    std::vector<char> counted;
    auto entry = [&](double target, double now, double most, double* l) {
        if (!std::isfinite(target) || !(now > 0) || !(most > 0)) return;
        lam.push_back(l);
        step.push_back(std::clamp(a_Vt * std::log(std::max(target, 1e-8 * most) / now), -3 * a_Vt, 3 * a_Vt));
        counted.push_back(std::max(target, now) > 1e-6 * most);
    };
    for (std::size_t k = 0; k < a_quantumNodes.size(); ++k) {
        const std::size_t i = std::size_t(a_quantumNodes[k]);
        if (a_quantumElectrons) entry(nq[k], f.values[N + i], mostN, &a_lamN[i]);
        if (a_quantumHoles) entry(pq[k], f.values[2 * N + i], mostP, &a_lamP[i]);
    }
    const int m = int(lam.size());
    double largest = 0;
    for (int j = 0; j < m; ++j)
        if (counted[std::size_t(j)]) largest = std::max(largest, std::abs(step[std::size_t(j)]));
    Eigen::VectorXd x(m), g(m);
    for (int j = 0; j < m; ++j) {
        x(j) = *lam[std::size_t(j)] / a_Vt;
        g(j) = counted[std::size_t(j)] ? step[std::size_t(j)] / a_Vt : 0.0;
    }
    Eigen::VectorXd next = x;
    for (int j = 0; j < m; ++j) next(j) += step[std::size_t(j)] / a_Vt;
    if (largest < a_Vt && a_andX.size() > 0 && a_andX.back().size() == m) {
        // Anderson: x + g - (ΔX + ΔG) γ, γ the least squares' of ΔG γ = g.
        const int depth = int(a_andX.size());
        Eigen::MatrixXd dX(m, depth), dG(m, depth);
        for (int c = 0; c < depth; ++c) {
            const Eigen::VectorXd& x1 = c + 1 < depth ? a_andX[std::size_t(c + 1)] : x;
            const Eigen::VectorXd& g1 = c + 1 < depth ? a_andG[std::size_t(c + 1)] : g;
            dX.col(c) = x1 - a_andX[std::size_t(c)];
            dG.col(c) = g1 - a_andG[std::size_t(c)];
        }
        const Eigen::VectorXd gamma = dG.colPivHouseholderQr().solve(g);
        if (gamma.allFinite()) {
            const Eigen::VectorXd mixed = x + g - (dX + dG) * gamma;
            for (int j = 0; j < m; ++j)
                if (counted[std::size_t(j)]) next(j) = x(j) + std::clamp(mixed(j) - x(j), -3.0, 3.0);
        }
    }
    if (largest < a_Vt) {
        a_andX.push_back(x);
        a_andG.push_back(g);
        if (a_andX.size() > 5) {
            a_andX.erase(a_andX.begin());
            a_andG.erase(a_andG.begin());
        }
    } else {
        a_andX.clear();
        a_andG.clear();
    }
    double change = 0;
    for (int j = 0; j < m; ++j) {
        const double before = *lam[std::size_t(j)];
        *lam[std::size_t(j)] = std::clamp(next(j) * a_Vt, -30 * a_Vt, 30 * a_Vt);
        if (counted[std::size_t(j)]) change = std::max(change, std::abs(*lam[std::size_t(j)] - before) / a_Vt);
    }
    for (std::size_t i = 0; i < N; ++i) {
        a_thetaN[i] = a_thetaN0[i] + a_lamN[i];
        a_thetaP[i] = a_thetaP0[i] - a_lamP[i];
    }
    // (Its measure of rest: the plain step's, how far the carriers are from
    // the states'.)
    return largest / a_Vt;
}

SemiconductorProblem::TrapState SemiconductorProblem::trapState(const TrapLevel& t, double n, double p)
{
    TrapState s;
    s.D = t.cn * (n + t.n1) + t.cp * (p + t.p1);
    const double filling = t.cn * n + t.cp * t.p1;
    s.f = filling / s.D;
    s.dfdn = t.cn * (t.cn * t.n1 + t.cp * p) / (s.D * s.D);
    s.dfdp = -t.cp * filling / (s.D * s.D);
    const double excess = n * p - t.n1 * t.p1;
    s.U = t.N * t.cn * t.cp * excess / s.D;
    s.dUdn = t.N * t.cn * t.cp * (p * s.D - excess * t.cn) / (s.D * s.D);
    s.dUdp = t.N * t.cn * t.cp * (n * s.D - excess * t.cp) / (s.D * s.D);
    return s;
}

void SemiconductorProblem::degeneracy(const Eigen::VectorXd& x) const
{
    if (!a_fermi) return;
    const FermiHalf& fh = fermiHalf();
    // γ = u/e^η at u = n/Nc: ln γ = ln u - η, d ln γ/dn = (1/u - 1/ℱ')/Nc;
    // few carriers, ln γ = -u/2^1.5 (Boltzmann's next term) - its derivative
    // there not a difference of 1/u's.
    auto one = [&](double c, double N, double* lg, double* dlg, double* r) {
        const double u = std::max(c, 0.0) / N;
        if (u < 1e-6) {
            *lg = -u / std::sqrt(8.0);
            *dlg = -1 / (std::sqrt(8.0) * N);
            *r = 1 - u / std::sqrt(8.0);
            return;
        }
        const double eta = fh.inverse(u);
        double F = 0, dF = 0;
        fh.at(eta, &F, &dF);
        *lg = std::log(u) - eta;
        *dlg = (1 / u - 1 / dF) / N;
        *r = dF / F;
    };
    for (int i = 0; i < a_n; ++i) {
        const int d = a_nodeDomain[std::size_t(i)];
        if (d < 0 || !a_media[std::size_t(d)].fermi) continue;
        const Medium& m = a_media[std::size_t(d)];
        const std::size_t k = std::size_t(i);
        one(x(a_iN[k]), m.Nc, &a_lgN[k], &a_dlgN[k], &a_rN[k]);
        one(x(a_iP[k]), m.Nv, &a_lgP[k], &a_dlgP[k], &a_rP[k]);
    }
}

void SemiconductorProblem::ionized(int i, double n, double p, double* nd, double* dnd, double* na, double* dna) const
{
    const double Nd = a_Nd[std::size_t(i)], Na = a_Na[std::size_t(i)];
    const int d = a_nodeDomain[std::size_t(i)];
    if (d < 0 || !a_media[std::size_t(d)].incomplete) {
        *nd = Nd;
        *na = Na;
        *dnd = *dna = 0;
        return;
    }
    const Medium& m = a_media[std::size_t(d)];
    const double un = 1 + m.gd * n / m.n1d, up = 1 + m.ga * p / m.p1a;
    *nd = Nd / un;
    *dnd = -Nd * m.gd / m.n1d / (un * un);
    *na = Na / up;
    *dna = -Na * m.ga / m.p1a / (up * up);
}

bool SemiconductorProblem::targets(std::vector<double>& out, QString* error)
{
    out.assign(a_contacts.size(), 0);
    PointEvaluator ev(a_s);
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        const Contact& c = a_contacts[k];
        if (!c.V0.isValid()) {
            *error = tr("%1: the voltage of %2 is not valid").arg(a_node->name(), c.name);
            return false;
        }
        double v = c.V0.isConstant() ? c.V0.constant() : NaN;
        if (!c.V0.isConstant()) {
            const auto& [t, r] = a_nodePoint[std::size_t(c.nodes.front())];
            ev.moveTo(t, r[0], r[1]);
            v = ev.eval(c.V0);
        }
        if (!std::isfinite(v)) {
            *error = tr("%1: the voltage of %2 is not a number").arg(a_node->name(), c.name);
            return false;
        }
        out[k] = v;
    }
    return true;
}

void SemiconductorProblem::contactValues(const std::vector<double>& applied)
{
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        const Contact& c = a_contacts[k];
        for (int i : c.nodes) {
            const double va = applied[k];
            if (c.kind == Contact::Gate) {
                // The metal's Fermi level at -q Va, its vacuum level Φm above.
                a_bcV[std::size_t(i)] = va - (c.phim - a_refChi - a_refDEc) / Q;
                continue;
            }
            const Medium& m = a_media[std::size_t(a_nodeDomain[std::size_t(i)])];
            if (c.kind == Contact::Schottky) {
                // (Its barrier to the band's edge as narrowed.)
                a_bcV[std::size_t(i)] = va + (m.dEc - a_gap[std::size_t(i)] / 2 - c.phiB) / Q - a_delta[std::size_t(i)];
                continue;
            }
            double n = 0, p = 0, v = 0;
            neutral(i, &n, &p, &v);
            a_bcN[std::size_t(i)] = n;
            a_bcP[std::size_t(i)] = p;
            a_bcV[std::size_t(i)] = va + a_Vt * v - a_delta[std::size_t(i)];
        }
    }
}

void SemiconductorProblem::edgeWeights(const Eigen::VectorXd* x)
{
    for (Edge& e : a_edges) e.wn = e.wp = 0;
    std::vector<double> geometric(a_edges.size(), 0.0), magnitude(a_edges.size(), 0.0);
    for (int t = 0; t < int(a_elements.size()); ++t) {
        const ElementMobility& em = a_elements[std::size_t(t)];
        if (em.domain < 0) continue;
        double mun = 0, mup = 0;
        elementMobilities(t, x, &mun, &mup);
        for (int k = 0; k < 3; ++k) {
            Edge& e = a_edges[std::size_t(em.edge[k])];
            e.wn += em.cot[k] * mun;
            e.wp += em.cot[k] * mup;
            geometric[std::size_t(em.edge[k])] += em.cot[k];
            magnitude[std::size_t(em.edge[k])] += std::abs(em.cot[k]);
        }
    }
    a_clamped = 0;
    for (std::size_t k = 0; k < a_edges.size(); ++k) {
        Edge& e = a_edges[k];
        // (Not Delaunay there: a negative weight would make the carriers
        // oscillate; it is left out. One of nought - a right angle across
        // a boundary's edge - is as it should be.)
        if (geometric[k] < -1e-9 * magnitude[k]) ++a_clamped;
        e.w = std::max(geometric[k], 0.0);
        e.wn = e.w > 0 ? std::max(e.wn, 0.0) * Q * a_Vt : 0.0;
        e.wp = e.w > 0 ? std::max(e.wp, 0.0) * Q * a_Vt : 0.0;
    }
}

void SemiconductorProblem::elementMobilities(int t, const Eigen::VectorXd* x, double* mun, double* mup) const
{
    const ElementMobility& em = a_elements[std::size_t(t)];
    *mun = em.mun * em.tn;
    *mup = em.mup * em.tp;
    const Medium& m = a_media[std::size_t(em.domain)];
    if (!x || (!m.saturation && !(m.surface && em.damp > 0))) return;
    // (Lagged a Newton step: the field and the carriers at x.)
    const int* dof = a_s.fields()[std::size_t(a_fi)].space->dofs(t);
    double V[3], ex = 0, ey = 0;
    for (int k = 0; k < 3; ++k) {
        V[k] = (*x)(a_iV[std::size_t(dof[k])]);
        ex -= em.gx[k] * V[k];
        ey -= em.gy[k] * V[k];
    }
    if (m.surface && em.damp > 0) {
        // 1/µ = 1/µbulk + D/µac + D/µsr (Matthiessen's rule), D = e^(-d/lcrit);
        // E⊥ no less than 1 V/cm.
        const double across = std::max(std::abs(ex * em.nx + ey * em.ny), 100.0);
        const double perCm3 = std::max(em.N * 1e-6, 1.0), vPerCm = across / 100;
        auto scatter = [&](double mu, double B, double C, double lambda, double delta) {
            const double ac = B / across + 1e-4 * C * std::pow(perCm3, lambda) / (a_T * std::cbrt(vPerCm));
            const double sr = delta / (across * across);
            return 1 / (1 / mu + em.damp / ac + em.damp / sr);
        };
        *mun = scatter(*mun, m.sBn, m.sCn, m.sLn, m.sDn);
        *mup = scatter(*mup, m.sBp, m.sCp, m.sLp, m.sDp);
    }
    if (m.saturation) {
        // µ(F) = µ0 / (1 + (µ0 F / vsat)^β)^(1/β), F the force along each
        // carrier's current: its quasi-Fermi potential's gradient, φn = V +
        // θn - Vt ln(n/ni), φp = V + θp + Vt ln(p/ni) - J/(q n µ), the field
        // along the current where it drifts, nought at equilibrium - no
        // more than the field: the field's work heats a carrier, and one
        // diffusing where there is little (a doped region's minority
        // carriers, their gradient steep) keeps its mobility. (Uncapped, the
        // photodiode example collected 98.5 % of its pairs, not 92.) Or the
        // whole field.
        // (E·J/|J| itself, J fitted to a triangle's three edge currents,
        // took an inversion layer's drift and diffusion across it - each
        // some 500 times the current along the channel, cancelling only in
        // all - for current: the channel's mobility halved.)
        const double whole = std::hypot(ex, ey);
        double Fn = whole, Fp = whole;
        if (!m.wholeField) {
            double gn[2] = {0, 0}, gp[2] = {0, 0};
            for (int k = 0; k < 3; ++k) {
                const std::size_t i = std::size_t(dof[k]);
                const double n = std::max((*x)(a_iN[i]), 1e-300), p = std::max((*x)(a_iP[i]), 1e-300);
                const double phin = V[k] + a_thetaN[i] + a_Vt * a_lgN[i] - a_Vt * std::log(n / a_refNi);
                const double phip = V[k] + a_thetaP[i] - a_Vt * a_lgP[i] + a_Vt * std::log(p / a_refNi);
                gn[0] += em.gx[k] * phin;
                gn[1] += em.gy[k] * phin;
                gp[0] += em.gx[k] * phip;
                gp[1] += em.gy[k] * phip;
            }
            Fn = std::min(whole, std::hypot(gn[0], gn[1]));
            Fp = std::min(whole, std::hypot(gp[0], gp[1]));
        }
        auto reduce = [](double mu, double F, double vsat, double beta) { return mu / std::pow(1 + std::pow(mu * F / vsat, beta), 1 / beta); };
        *mun = reduce(*mun, Fn, m.vsatn, m.betan);
        *mup = reduce(*mup, Fp, m.vsatp, m.betap);
    }
}

void SemiconductorProblem::updateMobilities(const Eigen::VectorXd& x)
{
    if (a_saturation || a_lombardi) edgeWeights(&x);
}

void SemiconductorProblem::assemble(const Eigen::VectorXd& x, double mass, const std::vector<double>* history, Eigen::VectorXd& F,
                                    std::vector<Eigen::Triplet<double>>* jac) const
{
    const Space& sp = *a_s.fields()[std::size_t(a_fi)].space;
    F = Eigen::VectorXd::Zero(a_unknowns);
    if (jac) jac->clear();
    degeneracy(x);
    auto add = [&](int r, int c, double v) {
        if (jac && r >= 0 && c >= 0) jac->emplace_back(r, c, v);
    };
    auto V = [&](int i) { return x(a_iV[std::size_t(i)]); };
    auto nn = [&](int i) { return a_iN[std::size_t(i)] >= 0 ? x(a_iN[std::size_t(i)]) : 0.0; };
    auto pp = [&](int i) { return a_iP[std::size_t(i)] >= 0 ? x(a_iP[std::size_t(i)]) : 0.0; };
    // Poisson: -Σ K V.
    for (int t = 0; t < sp.elements(); ++t) {
        if (!a_elemOn[std::size_t(t)]) continue;
        const int* dof = sp.dofs(t);
        const std::array<double, 9>& K = a_K[std::size_t(t)];
        for (int a = 0; a < 3; ++a) {
            const int r = a_iV[std::size_t(dof[a])];
            for (int b = 0; b < 3; ++b) {
                const double k = K[std::size_t(3 * a + b)];
                F(r) -= k * V(dof[b]);
                add(r, a_iV[std::size_t(dof[b])], -k);
            }
        }
    }
    // The carriers' currents along the edges (each carrier's potential step
    // the potential's and its band's offset).
    for (const Edge& e : a_edges) {
        if (e.w <= 0) continue;
        const int i = e.i, j = e.j;
        double dN = 0, dP = 0;
        edgeSteps(e, x, &dN, &dP);
        const int ni = a_iN[std::size_t(i)], nj = a_iN[std::size_t(j)], pi = a_iP[std::size_t(i)], pj = a_iP[std::size_t(j)];
        const int vi = a_iV[std::size_t(i)], vj = a_iV[std::size_t(j)];
        {
            const double bp = bern(dN), bm = bern(-dN), dbp = dbern(dN), dbm = dbern(-dN);
            const double fn = e.wn * (nn(j) * bp - nn(i) * bm);
            F(ni) += fn;
            F(nj) -= fn;
            // (Fermi-Dirac's: the step takes ln γ at each end, its change with
            // that end's n as the potential's, times Vt d ln γ/dn.)
            const double dvj = e.wn * (nn(j) * dbp + nn(i) * dbm) / a_Vt;
            const double dnj = e.wn * bp + dvj * a_Vt * a_dlgN[std::size_t(j)], dni = -e.wn * bm - dvj * a_Vt * a_dlgN[std::size_t(i)];
            for (const auto& [row, sign] : {std::pair{ni, 1.0}, std::pair{nj, -1.0}}) {
                add(row, nj, sign * dnj);
                add(row, ni, sign * dni);
                add(row, vj, sign * dvj);
                add(row, vi, -sign * dvj);
            }
        }
        {
            const double bp = bern(dP), bm = bern(-dP), dbp = dbern(dP), dbm = dbern(-dP);
            const double fp = e.wp * (pp(i) * bp - pp(j) * bm);
            F(pi) += fp;
            F(pj) -= fp;
            const double dvj = e.wp * (pp(i) * dbp + pp(j) * dbm) / a_Vt;
            const double dpi = e.wp * bp + dvj * a_Vt * a_dlgP[std::size_t(i)], dpj = -e.wp * bm - dvj * a_Vt * a_dlgP[std::size_t(j)];
            for (const auto& [row, sign] : {std::pair{pi, 1.0}, std::pair{pj, -1.0}}) {
                add(row, pi, sign * dpi);
                add(row, pj, sign * dpj);
                add(row, vj, sign * dvj);
                add(row, vi, -sign * dvj);
            }
        }
    }
    // Impact ionization, by triangle: its pairs (αn |Jn| + αp |Jp|)/q, the
    // α's at its field |E| (each edge's field along it is that projected -
    // a 45° edge's 0.71 E ionizes a seventh as much -, and each edge's
    // current that along it), Jn and Jp its three edges' currents along
    // them fitted (exact where the current is uniform); a third to each
    // corner's box.
    if (a_ionization) {
        const Space& isp = *a_s.fields()[std::size_t(a_fi)].space;
        // Each edge's current densities along it (A/m²) and their changes.
        struct EdgeCurrent {
            double jn = 0, jp = 0;
            double dn[4] = {0, 0, 0, 0}, dp[4] = {0, 0, 0, 0};   // by (n or p at i, at j, V at i, at j)
        };
        std::vector<EdgeCurrent> ec(a_edges.size());
        for (std::size_t k = 0; k < a_edges.size(); ++k) {
            const Edge& e = a_edges[k];
            if (e.w <= 0 || e.domain < 0 || !a_media[std::size_t(e.domain)].ionizationCurrents) continue;
            double dN = 0, dP = 0;
            edgeSteps(e, x, &dN, &dP);
            const double face = e.w * e.h;   // the dual face's area (m²)
            const double bnp = bern(dN), bnm = bern(-dN), bpp = bern(dP), bpm = bern(-dP);
            EdgeCurrent& c = ec[k];
            c.jn = e.wn * (nn(e.j) * bnp - nn(e.i) * bnm) / face;
            c.jp = e.wp * (pp(e.i) * bpp - pp(e.j) * bpm) / face;
            const double dnV = e.wn * (nn(e.j) * dbern(dN) + nn(e.i) * dbern(-dN)) / a_Vt / face;
            const double dpV = e.wp * (pp(e.i) * dbern(dP) + pp(e.j) * dbern(-dP)) / a_Vt / face;
            c.dn[0] = -e.wn * bnm / face;
            c.dn[1] = e.wn * bnp / face;
            c.dn[2] = -dnV;
            c.dn[3] = dnV;
            c.dp[0] = e.wp * bpp / face;
            c.dp[1] = -e.wp * bpm / face;
            c.dp[2] = -dpV;
            c.dp[3] = dpV;
        }
        for (int t = 0; t < isp.elements(); ++t) {
            if (!a_elemSemi[std::size_t(t)]) continue;
            const ElementMobility& em = a_elements[std::size_t(t)];
            const Medium& m = a_media[std::size_t(em.domain)];
            if (!m.ionization || !m.ionizationCurrents) continue;
            const int* dof = isp.dofs(t);
            // Its field.
            double Ex = 0, Ey = 0;
            for (int k = 0; k < 3; ++k) {
                Ex -= em.gx[k] * V(dof[k]);
                Ey -= em.gy[k] * V(dof[k]);
            }
            const double Ef = std::hypot(Ex, Ey);
            double an = 0, dan = 0, ap = 0, dap = 0;
            ionizationRate(Ef, m.an, m.bn, m.an2, m.bn2, m.E0, m.ionBetaN, &an, &dan);
            ionizationRate(Ef, m.ap, m.bp, m.ap2, m.bp2, m.E0, m.ionBetaP, &ap, &dap);
            if (an == 0 && ap == 0) continue;
            // Its currents: J = (Σ t tᵀ)⁻¹ Σ t j over its edges.
            double M[3] = {0, 0, 0};   // xx, xy, yy
            for (int k = 0; k < 3; ++k) {
                const Edge& e = a_edges[std::size_t(em.edge[k])];
                M[0] += e.tx * e.tx;
                M[1] += e.tx * e.ty;
                M[2] += e.ty * e.ty;
            }
            const double det = M[0] * M[2] - M[1] * M[1];
            if (!(std::abs(det) > 1e-12)) continue;
            // (A row of (Σ t tᵀ)⁻¹ t for edge k: how its j weighs in J.)
            double wx[3], wy[3];
            for (int k = 0; k < 3; ++k) {
                const Edge& e = a_edges[std::size_t(em.edge[k])];
                wx[k] = (M[2] * e.tx - M[1] * e.ty) / det;
                wy[k] = (-M[1] * e.tx + M[0] * e.ty) / det;
            }
            double Jnx = 0, Jny = 0, Jpx = 0, Jpy = 0;
            for (int k = 0; k < 3; ++k) {
                const EdgeCurrent& c = ec[std::size_t(em.edge[k])];
                Jnx += wx[k] * c.jn;
                Jny += wy[k] * c.jn;
                Jpx += wx[k] * c.jp;
                Jpy += wy[k] * c.jp;
            }
            const double Jn = std::hypot(Jnx, Jny), Jp = std::hypot(Jpx, Jpy);
            const double third = em.volume / 3;
            const double g = (an * Jn + ap * Jp) * third;   // A, to each corner
            for (int k = 0; k < 3; ++k) {
                F(a_iN[std::size_t(dof[k])]) += g;
                F(a_iP[std::size_t(dof[k])]) -= g;
            }
            if (!jac) continue;
            // Its change: by the field (each corner's V), by the currents
            // (each edge's ends' n, p, V).
            // (Its corners' V, n and p: nine unknowns at most.)
            std::array<std::pair<int, double>, 9> dg{};
            int used = 0;
            auto addTo = [&](int col, double d) {
                for (int u = 0; u < used; ++u)
                    if (dg[std::size_t(u)].first == col) {
                        dg[std::size_t(u)].second += d;
                        return;
                    }
                if (used < 9) dg[std::size_t(used++)] = {col, d};
            };
            if (Ef > 0)
                for (int k = 0; k < 3; ++k) {
                    const double dE = -(Ex * em.gx[k] + Ey * em.gy[k]) / Ef;
                    addTo(a_iV[std::size_t(dof[k])], (dan * Jn + dap * Jp) * dE * third);
                }
            for (int k = 0; k < 3; ++k) {
                const Edge& e = a_edges[std::size_t(em.edge[k])];
                const EdgeCurrent& c = ec[std::size_t(em.edge[k])];
                // d|J| = Ĵ·dJ, dJ = (wx, wy) dj.
                const double sn = Jn > 0 ? an * (Jnx * wx[k] + Jny * wy[k]) / Jn : 0.0;
                const double sp = Jp > 0 ? ap * (Jpx * wx[k] + Jpy * wy[k]) / Jp : 0.0;
                const int cols[4][2] = {{a_iN[std::size_t(e.i)], a_iP[std::size_t(e.i)]}, {a_iN[std::size_t(e.j)], a_iP[std::size_t(e.j)]},
                                        {a_iV[std::size_t(e.i)], a_iV[std::size_t(e.i)]}, {a_iV[std::size_t(e.j)], a_iV[std::size_t(e.j)]}};
                for (int q = 0; q < 4; ++q) {
                    addTo(cols[q][0], sn * c.dn[q] * third);
                    addTo(cols[q][1], sp * c.dp[q] * third);
                }
            }
            for (int k = 0; k < 3; ++k)
                for (int u = 0; u < used; ++u) {
                    add(a_iN[std::size_t(dof[k])], dg[std::size_t(u)].first, dg[std::size_t(u)].second);
                    add(a_iP[std::size_t(dof[k])], dg[std::size_t(u)].first, -dg[std::size_t(u)].second);
                }
        }
    }
    // At each semiconductor node: the space charge, recombination, generation,
    // the carriers' change in time.
    const std::size_t N = std::size_t(a_n);
    for (int i = 0; i < a_n; ++i) {
        const int d = a_nodeDomain[std::size_t(i)];
        if (d < 0) continue;
        const Medium& m = a_media[std::size_t(d)];
        const double A = a_box[std::size_t(i)];
        const int rv = a_iV[std::size_t(i)], rn = a_iN[std::size_t(i)], rp = a_iP[std::size_t(i)];
        const double n = nn(i), p = pp(i), ni = a_ni[std::size_t(i)];
        // (Fermi-Dirac's: γ = 1 for Boltzmann's.)
        const double gn = std::exp(a_lgN[std::size_t(i)]), gp = std::exp(a_lgP[std::size_t(i)]);
        if (m.incomplete) {
            double nd = 0, dnd = 0, na = 0, dna = 0;
            ionized(i, n / gn, p / gp, &nd, &dnd, &na, &dna);
            F(rv) += Q * A * (p - n + nd - na);
            add(rv, rn, Q * A * (dnd / gn - 1));
            add(rv, rp, Q * A * (1 - dna / gp));
        } else {
            F(rv) += Q * A * (p - n + doping(i));
            add(rv, rn, -Q * A);
            add(rv, rp, Q * A);
        }
        double R = 0, dRn = 0, dRp = 0;
        const double excess = n * p - ni * ni * gn * gp;
        if (m.srh) {
            const double D = m.taup * (n + gn * a_n1[std::size_t(i)]) + m.taun * (p + gp * a_p1[std::size_t(i)]);
            R += excess / D;
            dRn += (p * D - excess * m.taup) / (D * D);
            dRp += (n * D - excess * m.taun) / (D * D);
        }
        if (m.Cn != 0 || m.Cp != 0) {
            const double c = m.Cn * n + m.Cp * p;
            R += c * excess;
            dRn += m.Cn * excess + c * p;
            dRp += m.Cp * excess + c * n;
        }
        if (m.B != 0) {
            R += m.B * excess;
            dRn += m.B * p;
            dRp += m.B * n;
        }
        const double G = a_generation.empty() ? 0.0 : a_generation[std::size_t(i)];
        F(rn) -= Q * A * (R - G);
        F(rp) += Q * A * (R - G);
        add(rn, rn, -Q * A * dRn);
        add(rn, rp, -Q * A * dRp);
        add(rp, rn, Q * A * dRn);
        add(rp, rp, Q * A * dRp);
        if (mass > 0 && history) {
            F(rn) -= Q * A * (mass * n - (*history)[N + std::size_t(i)]);
            F(rp) += Q * A * (mass * p - (*history)[2 * N + std::size_t(i)]);
            add(rn, rn, -Q * A * mass);
            add(rp, rp, Q * A * mass);
        }
    }
    // Surface charges.
    for (std::size_t k = 0; k < a_surface.size(); ++k) {
        const int i = a_surface[k].first;
        const Expression& e = a_surface[k].second;
        double rho = 0;
        if (e.isConstant()) rho = e.constant();
        else {
            PointEvaluator ev(a_s);
            const auto& [t, r] = a_nodePoint[std::size_t(i)];
            ev.moveTo(t, r[0], r[1]);
            rho = ev.eval(e);
        }
        if (std::isfinite(rho)) F(a_iV[std::size_t(i)]) += rho * a_surfaceLength[k];
    }
    // Interface traps: each level's charge, -q N f (acceptor-like) or
    // q N (1 - f), and the pairs it recombines, at Boltzmann's carriers
    // (Fermi-Dirac's n/γ: the traps filled by the Fermi level).
    for (const TrapLevel& tl : a_traps) {
        const int i = tl.node;
        const std::size_t k = std::size_t(i);
        const double gn = std::exp(a_lgN[k]), gp = std::exp(a_lgP[k]);
        const TrapState st = trapState(tl, nn(i) / gn, pp(i) / gp);
        const int rv = a_iV[k], rn = a_iN[k], rp = a_iP[k];
        F(rv) += Q * tl.N * (tl.acceptor ? -st.f : 1 - st.f) * tl.area;
        add(rv, rn, -Q * tl.N * st.dfdn / gn * tl.area);
        add(rv, rp, -Q * tl.N * st.dfdp / gp * tl.area);
        F(rn) -= Q * tl.area * st.U;
        F(rp) += Q * tl.area * st.U;
        add(rn, rn, -Q * tl.area * st.dUdn / gn);
        add(rn, rp, -Q * tl.area * st.dUdp / gp);
        add(rp, rn, Q * tl.area * st.dUdn / gn);
        add(rp, rp, Q * tl.area * st.dUdp / gp);
    }
    // Schottky contacts: thermionic emission over the barrier.
    for (const Contact& c : a_contacts) {
        if (c.kind != Contact::Schottky) continue;
        for (std::size_t k = 0; k < c.nodes.size(); ++k) {
            const int i = c.nodes[k];
            const Medium& m = a_media[std::size_t(a_nodeDomain[std::size_t(i)])];
            const double ni = a_ni[std::size_t(i)];
            const double neq = m.Nc * std::exp(-c.phiB / (KB * a_T)), peq = ni * ni / neq;
            const double L = c.length[k];
            const int rn = a_iN[std::size_t(i)], rp = a_iP[std::size_t(i)];
            F(rn) -= Q * c.vn * (nn(i) - neq) * L;
            add(rn, rn, -Q * c.vn * L);
            F(rp) += Q * c.vp * (pp(i) - peq) * L;
            add(rp, rp, Q * c.vp * L);
        }
    }
}

bool SemiconductorProblem::newton(Eigen::VectorXd& x, double mass, const std::vector<double>* history, int maxSteps, int* steps, QString* why)
{
    // Unknowns in units of Vt and of each node's own carriers (a relative
    // change: minority carriers 10^14 below the majority keep their digits);
    // each row divided by its largest entry.
    Eigen::VectorXd colScale(a_unknowns);
    auto carrierScales = [&]() {
        for (int i = 0; i < a_n; ++i) {
            if (a_iV[std::size_t(i)] >= 0) colScale(a_iV[std::size_t(i)]) = a_Vt;
            for (int k : {a_iN[std::size_t(i)], a_iP[std::size_t(i)]})
                if (k >= 0) colScale(k) = std::max(std::abs(x(k)), 1e-20 * a_scale);
        }
    };
    std::vector<char> fixed(std::size_t(a_unknowns), 0);
    Eigen::VectorXd target = Eigen::VectorXd::Zero(a_unknowns);
    for (const Contact& c : a_contacts)
        for (int i : c.nodes) {
            fixed[std::size_t(a_iV[std::size_t(i)])] = 1;
            target(a_iV[std::size_t(i)]) = a_bcV[std::size_t(i)];
            if (c.kind == Contact::Ohmic) {
                fixed[std::size_t(a_iN[std::size_t(i)])] = fixed[std::size_t(a_iP[std::size_t(i)])] = 1;
                target(a_iN[std::size_t(i)]) = a_bcN[std::size_t(i)];
                target(a_iP[std::size_t(i)]) = a_bcP[std::size_t(i)];
            }
        }
    for (int k = 0; k < a_unknowns; ++k)
        if (fixed[std::size_t(k)]) x(k) = target(k);
    Eigen::VectorXd F;
    std::vector<Eigen::Triplet<double>> trip;
    Eigen::SparseLU<Eigen::SparseMatrix<double>> lu;
    bool analyzed = false;
    int crawling = 0;   // steps in a row damped to less than 2 %
    // (Mobilities saturated by the force along the current: lagged a step,
    // they settle only as the carriers do - where generation sets a
    // current, J/(q n µ), a lower µ a steeper force and a lower µ again, at
    // (v/vsat)² a step; under a MOSFET's channel, carriers that carry
    // nothing cycling about 1e-4. Held once the potential has settled to
    // 1e-6 Vt, or after fifteen steps, Newton finishing with them.)
    bool held = false;
    for (int it = 0; it < maxSteps; ++it) {
        carrierScales();
        if (!held) updateMobilities(x);
        assemble(x, mass, history, F, &trip);
        // Dirichlet rows: x - target.
        std::vector<Eigen::Triplet<double>> kept;
        kept.reserve(trip.size() + std::size_t(a_unknowns));
        for (const auto& t : trip)
            if (!fixed[std::size_t(t.row())]) kept.emplace_back(t.row(), t.col(), t.value() * colScale(t.col()));
        for (int k = 0; k < a_unknowns; ++k)
            if (fixed[std::size_t(k)]) {
                kept.emplace_back(k, k, colScale(k));
                F(k) = x(k) - target(k);
            }
        Eigen::SparseMatrix<double> J(a_unknowns, a_unknowns);
        J.setFromTriplets(kept.begin(), kept.end());
        // Row equilibration.
        Eigen::VectorXd rowMax = Eigen::VectorXd::Zero(a_unknowns);
        for (int k = 0; k < J.outerSize(); ++k)
            for (Eigen::SparseMatrix<double>::InnerIterator itr(J, k); itr; ++itr)
                rowMax(itr.row()) = std::max(rowMax(itr.row()), std::abs(itr.value()));
        for (int k = 0; k < a_unknowns; ++k)
            if (!(rowMax(k) > 0)) rowMax(k) = 1;
        for (int k = 0; k < J.outerSize(); ++k)
            for (Eigen::SparseMatrix<double>::InnerIterator itr(J, k); itr; ++itr) itr.valueRef() /= rowMax(itr.row());
        const Eigen::VectorXd rhs = -(F.array() / rowMax.array()).matrix();
        J.makeCompressed();
        if (!analyzed) {
            lu.analyzePattern(J);
            analyzed = true;
        }
        lu.factorize(J);
        if (lu.info() != Eigen::Success) {
            *why = tr("%1: its Jacobian is singular (%2)").arg(a_node->name(), QString::fromStdString(lu.lastErrorMessage()));
            return false;
        }
        Eigen::VectorXd dx = lu.solve(rhs);
        if (!dx.allFinite()) {
            *why = tr("%1: Newton's step is not finite").arg(a_node->name());
            return false;
        }
        dx = (dx.array() * colScale.array()).matrix();
        // Damping: the potential moved no more than 10 Vt at once; the
        // carriers kept positive.
        double maxDV = 0;
        for (int i = 0; i < a_n; ++i)
            if (a_iV[std::size_t(i)] >= 0) maxDV = std::max(maxDV, std::abs(dx(a_iV[std::size_t(i)])));
        const double alpha = maxDV > 10 * a_Vt ? 10 * a_Vt / maxDV : 1.0;
        double changeV = 0, changeC = 0;
        for (int i = 0; i < a_n; ++i) {
            if (a_iV[std::size_t(i)] >= 0) {
                const int k = a_iV[std::size_t(i)];
                x(k) += alpha * dx(k);
                changeV = std::max(changeV, std::abs(alpha * dx(k)) / a_Vt);
            }
            for (int k : {a_iN[std::size_t(i)], a_iP[std::size_t(i)]}) {
                if (k < 0) continue;
                const double before = x(k);
                double after = before + alpha * dx(k);
                if (after < 0.05 * before) after = 0.05 * before;
                x(k) = after;
                changeC = std::max(changeC, std::abs(after - before) / (before + 1e-10 * a_ni[std::size_t(i)] + 1e-300));
            }
        }
        if (steps) *steps = it + 1;
        // Ten steps in a row crawling: it is going nowhere - said at once,
        // the voltage's step taken again smaller (its forty steps were a
        // breakdown's diode's most time).
        crawling = alpha < 0.02 ? crawling + 1 : 0;
        if (crawling >= 10) break;
        held = held || (a_alongCurrent && alpha == 1.0 && (changeV < 1e-6 || it >= 15));
        // Converged: the potential still to 1e-8 Vt and the carriers to 1e-6
        // (or the potential to 1e-12 Vt: the carriers' last digits are noise).
        if (alpha == 1.0 && changeV < 1e-8 && (changeC < 1e-6 || changeV < 1e-12)) return true;
    }
    *why = tr("%1: Newton's method did not converge in %2 steps").arg(a_node->name()).arg(maxSteps);
    return false;
}

bool SemiconductorProblem::atEquilibrium(const std::vector<double>& goal, double* fermi) const
{
    for (double g : a_generation)
        if (g != 0) return false;
    bool any = false;
    double v = 0;
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        if (a_contacts[k].kind == Contact::Gate) continue;
        if (!any) v = goal[k];
        else if (std::abs(goal[k] - v) > 1e-12) return false;
        any = true;
    }
    *fermi = v;
    return any;
}

bool SemiconductorProblem::equilibriumPoisson(Eigen::VectorXd& x, double fermi, const std::vector<double>& applied, bool warm, QString* why)
{
    // V alone; n = ni exp((V + δ - φ)/Vt), p = ni exp((φ - V - δ)/Vt).
    contactValues(applied);
    if (!warm)
        for (int i = 0; i < a_n; ++i) {
            if (a_iV[std::size_t(i)] < 0) continue;
            double v = fermi;
            if (a_nodeDomain[std::size_t(i)] >= 0) v += a_Vt * std::asinh(doping(i) / (2 * a_ni[std::size_t(i)])) - a_delta[std::size_t(i)];
            x(a_iV[std::size_t(i)]) = v;
        }
    std::vector<int> vIndex;
    std::vector<int> local(std::size_t(a_unknowns), -1);
    for (int i = 0; i < a_n; ++i)
        if (a_iV[std::size_t(i)] >= 0) {
            local[std::size_t(a_iV[std::size_t(i)])] = int(vIndex.size());
            vIndex.push_back(i);
        }
    const int nv = int(vIndex.size());
    std::vector<char> fixed(std::size_t(nv), 0);
    for (const Contact& c : a_contacts)
        for (int i : c.nodes) {
            const int k = local[std::size_t(a_iV[std::size_t(i)])];
            fixed[std::size_t(k)] = 1;
            x(a_iV[std::size_t(i)]) = a_bcV[std::size_t(i)];
        }
    Eigen::SparseLU<Eigen::SparseMatrix<double>> lu;
    bool analyzed = false;
    for (int it = 0; it < 200; ++it) {
        Eigen::VectorXd Fv;
        Eigen::SparseMatrix<double> J;
        equilibriumSystem(x, fermi, local, vIndex, fixed, false, Fv, J);
        if (!analyzed) {
            lu.analyzePattern(J);
            analyzed = true;
        }
        lu.factorize(J);
        if (lu.info() != Eigen::Success) {
            *why = tr("%1: the equilibrium's potential: a singular matrix").arg(a_node->name());
            return false;
        }
        const Eigen::VectorXd dv = lu.solve(-Fv);
        double maxDV = 0;
        for (int k = 0; k < nv; ++k) maxDV = std::max(maxDV, std::abs(dv(k)));
        const double alpha = maxDV > 5 * a_Vt ? 5 * a_Vt / maxDV : 1.0;
        for (int k = 0; k < nv; ++k) x(a_iV[std::size_t(vIndex[std::size_t(k)])]) += alpha * dv(k);
        if (alpha == 1.0 && maxDV < 1e-10 * a_Vt) {
            for (int i = 0; i < a_n; ++i) {
                if (a_iN[std::size_t(i)] < 0) continue;
                const double v = (x(a_iV[std::size_t(i)]) + a_delta[std::size_t(i)] - fermi) / a_Vt;
                double dn = 0, dp = 0;
                carriersAt(i, v, &x(a_iN[std::size_t(i)]), &x(a_iP[std::size_t(i)]), &dn, &dp);
            }
            return true;
        }
    }
    *why = tr("%1: no equilibrium potential found in 200 steps").arg(a_node->name());
    return false;
}

void SemiconductorProblem::equilibriumSystem(const Eigen::VectorXd& x0, double fermi, const std::vector<int>& local, const std::vector<int>& vIndex,
                                             const std::vector<char>& fixed, bool keepContacts, Eigen::VectorXd& Fv,
                                             Eigen::SparseMatrix<double>& J) const
{
    // The carriers from V; the full assembly's Poisson rows give F and ∂F/∂V
    // (with ∂/∂n, ∂/∂p folded in through dn/dV, dp/dV: Boltzmann's n/Vt,
    // -p/Vt; Fermi-Dirac's Nc ℱ'/Vt, -Nv ℱ'/Vt).
    Eigen::VectorXd x = x0;
    std::vector<double> dnv(std::size_t(a_n), 0.0), dpv(std::size_t(a_n), 0.0);
    for (int i = 0; i < a_n; ++i) {
        if (a_iN[std::size_t(i)] < 0) continue;
        const double v = (x(a_iV[std::size_t(i)]) + a_delta[std::size_t(i)] - fermi) / a_Vt;
        carriersAt(i, v, &x(a_iN[std::size_t(i)]), &x(a_iP[std::size_t(i)]), &dnv[std::size_t(i)], &dpv[std::size_t(i)]);
    }
    Eigen::VectorXd F;
    std::vector<Eigen::Triplet<double>> trip;
    assemble(x, 0, nullptr, F, &trip);
    const int nv = int(vIndex.size());
    Fv.resize(nv);
    for (int k = 0; k < nv; ++k) Fv(k) = fixed[std::size_t(k)] && !keepContacts ? 0.0 : F(a_iV[std::size_t(vIndex[std::size_t(k)])]);
    std::vector<Eigen::Triplet<double>> kept;
    for (const auto& t : trip) {
        const int r = local[std::size_t(t.row())];
        if (r < 0 || (fixed[std::size_t(r)] && !keepContacts)) continue;
        int c = local[std::size_t(t.col())];
        double v = t.value();
        if (c < 0) {
            // ∂/∂n or ∂/∂p of this node's V row: through n(V), p(V).
            const int node = vIndex[std::size_t(r)];
            const bool isN = a_iN[std::size_t(node)] == t.col();
            if (!isN && a_iP[std::size_t(node)] != t.col()) continue;
            v *= (isN ? dnv[std::size_t(node)] : dpv[std::size_t(node)]) / a_Vt;
            c = r;
        }
        kept.emplace_back(r, c, v);
    }
    if (!keepContacts)
        for (int k = 0; k < nv; ++k)
            if (fixed[std::size_t(k)]) kept.emplace_back(k, k, 1.0);
    J.resize(nv, nv);
    J.setFromTriplets(kept.begin(), kept.end());
    J.makeCompressed();
}

void SemiconductorProblem::quasiStatic(const Eigen::VectorXd& x, double fermi, const std::vector<double>& applied)
{
    a_C0.clear();
    a_gates.clear();
    for (int k = 0; k < int(a_contacts.size()); ++k)
        if (a_contacts[std::size_t(k)].kind == Contact::Gate) a_gates.push_back(k);
    if (a_gates.empty()) return;
    contactValues(applied);
    std::vector<int> vIndex;
    std::vector<int> local(std::size_t(a_unknowns), -1);
    for (int i = 0; i < a_n; ++i)
        if (a_iV[std::size_t(i)] >= 0) {
            local[std::size_t(a_iV[std::size_t(i)])] = int(vIndex.size());
            vIndex.push_back(i);
        }
    const int nv = int(vIndex.size());
    std::vector<char> fixed(std::size_t(nv), 0);
    for (const Contact& c : a_contacts)
        for (int i : c.nodes) fixed[std::size_t(local[std::size_t(a_iV[std::size_t(i)])])] = 1;
    Eigen::VectorXd Fv;
    Eigen::SparseMatrix<double> J, full;
    equilibriumSystem(x, fermi, local, vIndex, fixed, false, Fv, J);
    equilibriumSystem(x, fermi, local, vIndex, fixed, true, Fv, full);
    Eigen::SparseLU<Eigen::SparseMatrix<double>> lu;
    lu.compute(J);
    if (lu.info() != Eigen::Success) return;
    // Each gate at 1 V more, the other contacts held: each gate's charge.
    const Eigen::SparseMatrix<double, Eigen::RowMajor> rows = full;
    a_C0.assign(a_gates.size(), std::vector<double>(a_gates.size(), 0.0));
    for (std::size_t b = 0; b < a_gates.size(); ++b) {
        Eigen::VectorXd rhs = Eigen::VectorXd::Zero(nv);
        for (int i : a_contacts[std::size_t(a_gates[b])].nodes) rhs(local[std::size_t(a_iV[std::size_t(i)])]) = 1;
        const Eigen::VectorXd dv = lu.solve(rhs);
        for (std::size_t a = 0; a < a_gates.size(); ++a) {
            double dq = 0;
            for (int i : a_contacts[std::size_t(a_gates[a])].nodes) {
                const int r = local[std::size_t(a_iV[std::size_t(i)])];
                for (Eigen::SparseMatrix<double, Eigen::RowMajor>::InnerIterator it(rows, r); it; ++it) dq -= it.value() * dv(it.col());
            }
            a_C0[a][b] = dq;
        }
    }
}

Eigen::VectorXd SemiconductorProblem::stateVector() const
{
    const Field& f = a_s.fields()[std::size_t(a_fi)];
    const std::size_t N = std::size_t(a_n);
    Eigen::VectorXd x = Eigen::VectorXd::Zero(a_unknowns);
    for (int i = 0; i < a_n; ++i) {
        if (a_iV[std::size_t(i)] >= 0) x(a_iV[std::size_t(i)]) = f.values[std::size_t(i)];
        if (a_iN[std::size_t(i)] >= 0) x(a_iN[std::size_t(i)]) = f.values[N + std::size_t(i)];
        if (a_iP[std::size_t(i)] >= 0) x(a_iP[std::size_t(i)]) = f.values[2 * N + std::size_t(i)];
    }
    return x;
}

void SemiconductorProblem::store(const Eigen::VectorXd& x)
{
    Field& f = field();
    const std::size_t N = std::size_t(a_n);
    degeneracy(x);
    for (int i = 0; i < a_n; ++i) {
        f.values[std::size_t(i)] = a_iV[std::size_t(i)] >= 0 ? x(a_iV[std::size_t(i)]) : 0.0;
        f.values[N + std::size_t(i)] = a_iN[std::size_t(i)] >= 0 ? x(a_iN[std::size_t(i)]) : 0.0;
        f.values[2 * N + std::size_t(i)] = a_iP[std::size_t(i)] >= 0 ? x(a_iP[std::size_t(i)]) : 0.0;
        // (The bands' offsets with Fermi-Dirac's degeneracy and the quantum
        // potentials in them: φn, φp and the currents read at a point as
        // solved.)
        if (a_fermi || !a_quantumNodes.empty()) {
            f.values[5 * N + std::size_t(i)] = a_thetaN[std::size_t(i)] + a_Vt * a_lgN[std::size_t(i)];
            f.values[6 * N + std::size_t(i)] = a_thetaP[std::size_t(i)] - a_Vt * a_lgP[std::size_t(i)];
        }
    }
    f.solved = true;
    // The carriers' Joule heat by triangle: each edge's currents times their
    // quasi-Fermi potentials' drop along it - the edges' sum the contacts'
    // I V less the pairs' recombination heat, exactly - shared among the
    // edge's triangles as their conductances are, over each one's volume.
    // (J²/(q n µ) at a point took a depletion region's drift and diffusion,
    // cancelling, for current: its heat thousands of times too much.)
    {
        std::vector<double> phin(N, 0.0), phip(N, 0.0);
        for (std::size_t i = 0; i < N; ++i) {
            if (a_iN[i] < 0) continue;
            phin[i] = x(a_iV[i]) + a_thetaN[i] + a_Vt * a_lgN[i] - a_Vt * std::log(std::max(x(a_iN[i]), 1e-300) / a_refNi);
            phip[i] = x(a_iV[i]) + a_thetaP[i] - a_Vt * a_lgP[i] + a_Vt * std::log(std::max(x(a_iP[i]), 1e-300) / a_refNi);
        }
        std::vector<double> pn(a_edges.size(), 0.0), pp(a_edges.size(), 0.0);
        for (std::size_t k = 0; k < a_edges.size(); ++k) {
            const Edge& e = a_edges[k];
            if (e.w <= 0) continue;
            double dN = 0, dP = 0;
            edgeSteps(e, x, &dN, &dP);
            const std::size_t i = std::size_t(e.i), j = std::size_t(e.j);
            const double fn = e.wn * (x(a_iN[j]) * bern(dN) - x(a_iN[i]) * bern(-dN));
            const double fp = e.wp * (x(a_iP[i]) * bern(dP) - x(a_iP[j]) * bern(-dP));
            pn[k] = fn * (phin[i] - phin[j]);
            pp[k] = fp * (phip[i] - phip[j]);
        }
        for (int t = 0; t < int(a_elements.size()); ++t) {
            const ElementMobility& em = a_elements[std::size_t(t)];
            if (em.domain < 0 || f.extra.size() < std::size_t(ExtraHead + PerTriangle * (t + 1)) || !(em.volume > 0)) continue;
            double mun = 0, mup = 0;
            elementMobilities(t, &x, &mun, &mup);
            double heat = 0;
            for (int k = 0; k < 3; ++k) {
                const Edge& e = a_edges[std::size_t(em.edge[k])];
                if (e.wn > 0) heat += em.cot[k] * mun * Q * a_Vt / e.wn * pn[std::size_t(em.edge[k])];
                if (e.wp > 0) heat += em.cot[k] * mup * Q * a_Vt / e.wp * pp[std::size_t(em.edge[k])];
            }
            f.extra[std::size_t(ExtraHead + PerTriangle * t + 18)] = heat / em.volume;
        }
    }
    // The mobilities as solved, for what is read at a point (semi.mun, the
    // current densities).
    if (a_saturation || a_lombardi || a_TL.isValid())
        for (int t = 0; t < int(a_elements.size()); ++t) {
            if (a_elements[std::size_t(t)].domain < 0 || f.extra.size() < std::size_t(ExtraHead + PerTriangle * (t + 1))) continue;
            double* e = &f.extra[std::size_t(ExtraHead + PerTriangle * t)];
            elementMobilities(t, &x, &e[0], &e[1]);
        }
    for (std::size_t k = 0; k < a_contacts.size() && k < f.terminals.size(); ++k) {
        f.terminals[k].potential = a_contacts[k].applied;
        f.terminals[k].charge = a_contacts[k].current;
        f.terminals[k].chargeIm = a_contacts[k].charge;
    }
}

std::vector<std::vector<double>> SemiconductorProblem::currentWeights(const Eigen::VectorXd& x)
{
    const std::size_t nc = a_contacts.size();
    std::vector<std::vector<double>> weight(nc);
    // Held: each node of a contact that holds carriers (ohmic, Schottky);
    // the semiconductor's other nodes free.
    std::vector<int> held(std::size_t(a_n), -1), local(std::size_t(a_n), -1);
    std::vector<int> columns(nc, -1);
    int carriersHeld = 0;
    for (std::size_t k = 0; k < nc; ++k) {
        if (a_contacts[k].kind == Contact::Gate) continue;
        columns[k] = carriersHeld++;
        for (int i : a_contacts[k].nodes) held[std::size_t(i)] = int(k);
    }
    int free = 0;
    for (int i = 0; i < a_n; ++i)
        if (a_nodeDomain[std::size_t(i)] >= 0 && held[std::size_t(i)] < 0) local[std::size_t(i)] = free++;
    // Each edge's conductance: its currents' change with the quasi-Fermi
    // potentials' difference (n e^(..) B weighed), relative to the largest
    // and at least 10^-12 of it (a junction's depletion has less: the
    // weight's step is there all the same).
    std::vector<double> g(a_edges.size(), 0.0);
    double largest = 0;
    for (std::size_t k = 0; k < a_edges.size(); ++k) {
        const Edge& e = a_edges[k];
        if (e.w <= 0) continue;
        const int i = e.i, j = e.j;
        double dN = 0, dP = 0;
        edgeSteps(e, x, &dN, &dP);
        const double ni = x(a_iN[std::size_t(i)]), nj = x(a_iN[std::size_t(j)]), pi = x(a_iP[std::size_t(i)]), pj = x(a_iP[std::size_t(j)]);
        g[k] = e.wn * 0.5 * (ni * bern(-dN) + nj * bern(dN)) + e.wp * 0.5 * (pi * bern(dP) + pj * bern(-dP));
        if (std::isfinite(g[k])) largest = std::max(largest, g[k]);
    }
    for (double& v : g) v = largest > 0 && std::isfinite(v) ? std::max(v / largest, 1e-12) : 1e-12;
    Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(free, std::max(carriersHeld, 1));
    if (free > 0 && carriersHeld > 0) {
        std::vector<Eigen::Triplet<double>> trip;
        trip.reserve(4 * a_edges.size() + std::size_t(free));
        for (std::size_t k = 0; k < a_edges.size(); ++k) {
            const int i = a_edges[k].i, j = a_edges[k].j;
            const int li = local[std::size_t(i)], lj = local[std::size_t(j)];
            const double gk = g[k];
            if (li >= 0) trip.emplace_back(li, li, gk);
            if (lj >= 0) trip.emplace_back(lj, lj, gk);
            if (li >= 0 && lj >= 0) {
                trip.emplace_back(li, lj, -gk);
                trip.emplace_back(lj, li, -gk);
            } else if (li >= 0 && held[std::size_t(j)] >= 0) {
                rhs(li, columns[std::size_t(held[std::size_t(j)])]) += gk;
            } else if (lj >= 0 && held[std::size_t(i)] >= 0) {
                rhs(lj, columns[std::size_t(held[std::size_t(i)])]) += gk;
            }
        }
        // (A little to nought everywhere: a region no contact reaches is
        // weighed nought, not singular.)
        for (int k = 0; k < free; ++k) trip.emplace_back(k, k, 1e-14);
        Eigen::SparseMatrix<double> L(free, free);
        L.setFromTriplets(trip.begin(), trip.end());
        if (a_weightSize != free) {
            a_weightSolver.analyzePattern(L);
            a_weightSize = free;
        }
        a_weightSolver.factorize(L);
        if (a_weightSolver.info() == Eigen::Success) rhs = a_weightSolver.solve(rhs);
        else rhs.setZero();
    }
    for (std::size_t k = 0; k < nc; ++k) {
        if (columns[k] < 0) continue;
        std::vector<double>& w = weight[k];
        w.assign(std::size_t(a_n), 0.0);
        for (int i = 0; i < a_n; ++i) {
            if (held[std::size_t(i)] >= 0) w[std::size_t(i)] = held[std::size_t(i)] == int(k) ? 1.0 : 0.0;
            else if (local[std::size_t(i)] >= 0) w[std::size_t(i)] = std::clamp(rhs(local[std::size_t(i)], columns[k]), 0.0, 1.0);
        }
    }
    return weight;
}

void SemiconductorProblem::terminalValues(const Eigen::VectorXd& x, double mass, const std::vector<double>* history, bool still)
{
    Eigen::VectorXd F;
    assemble(x, mass, history, F, nullptr);
    for (Contact& c : a_contacts) {
        double Qc = 0;
        for (int i : c.nodes) Qc -= F(a_iV[std::size_t(i)]);
        c.charge = Qc;
        c.current = 0;
    }
    // At equilibrium no current flows: nought, not the rounding of the
    // contacts' fluxes (-291 pA on a 10^19 cm^-3 one).
    if (still) return;
    // Each contact's: Σ over the edges of their currents times its weight's
    // change, and the carriers' change in time where it weighs (the
    // recombination's share is the electrons' and the holes' alike: none).
    const std::vector<std::vector<double>> weight = currentWeights(x);
    const std::size_t N = std::size_t(a_n);
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        const std::vector<double>& w = weight[k];
        if (w.empty()) continue;
        double I = 0;
        for (const Edge& e : a_edges) {
            if (e.w <= 0) continue;
            const double dw = w[std::size_t(e.i)] - w[std::size_t(e.j)];
            if (dw == 0) continue;
            const int i = e.i, j = e.j;
            double dN = 0, dP = 0;
            edgeSteps(e, x, &dN, &dP);
            const double fn = e.wn * (x(a_iN[std::size_t(j)]) * bern(dN) - x(a_iN[std::size_t(i)]) * bern(-dN));
            const double fp = e.wp * (x(a_iP[std::size_t(i)]) * bern(dP) - x(a_iP[std::size_t(j)]) * bern(-dP));
            I += (fn + fp) * dw;
        }
        if (mass > 0 && history)
            for (int i = 0; i < a_n; ++i) {
                if (w[std::size_t(i)] == 0 || a_iN[std::size_t(i)] < 0) continue;
                const double A = a_box[std::size_t(i)];
                const double dn = mass * x(a_iN[std::size_t(i)]) - (*history)[N + std::size_t(i)];
                const double dp = mass * x(a_iP[std::size_t(i)]) - (*history)[2 * N + std::size_t(i)];
                I += w[std::size_t(i)] * Q * A * (dp - dn);
            }
        a_contacts[k].current = I;
    }
}

void SemiconductorProblem::ionizationIntegrals(const Eigen::VectorXd& x)
{
    a_In = a_Ip = a_Emax = a_ionLength = NaN;
    const Space& sp = *field().space;
    const Mesh& mesh = a_s.mesh();
    const double unit = mesh.unitScale;
    auto ionizes = [&](int t) {
        if (t < 0 || !a_elemSemi[std::size_t(t)]) return false;
        return a_media[std::size_t(mesh.domain[std::size_t(t)])].ionization;
    };
    // Each triangle's field, -∇V; recovered at the nodes (their triangles'
    // by area, a domain's own), straight between them within a triangle: a
    // junction's node on a boundary, its doping the mean of fewer
    // triangles', makes one triangle's field a few percent high - the
    // integrals go as the field to the fifth, and a line from there gave
    // 37 % more.
    auto fieldIn = [&](int t) {
        const ElementMobility& em = a_elements[std::size_t(t)];
        const int* dof = sp.dofs(t);
        QPointF E(0, 0);
        for (int k = 0; k < 3; ++k) {
            const double v = x(a_iV[std::size_t(dof[k])]);
            E -= QPointF(em.gx[k] * v, em.gy[k] * v);
        }
        return E;
    };
    auto corner = [&](int t, int k) { return mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * unit; };
    // (Each domain's own at a node: across a junction one side's field is
    // not the other's - a p⁺ side's depletion, thinner than a triangle,
    // would pull the junction's node down.)
    std::map<std::pair<int, int>, std::pair<QPointF, double>> recovered;   // (node, domain): Σ area E, Σ area
    for (int t = 0; t < sp.elements(); ++t) {
        if (!ionizes(t)) continue;
        const QPointF a = corner(t, 0), b = corner(t, 1), c = corner(t, 2);
        const double area = std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2;
        const QPointF E = fieldIn(t);
        const int* dof = sp.dofs(t);
        for (int k = 0; k < 3; ++k) {
            auto& [sum, w] = recovered[{dof[k], mesh.domain[std::size_t(t)]}];
            sum += area * E;
            w += area;
        }
    }
    auto nodalIn = [&](int t, int k) {
        const auto it = recovered.find({sp.dofs(t)[k], mesh.domain[std::size_t(t)]});
        return it == recovered.end() || !(it->second.second > 0) ? QPointF(0, 0) : it->second.first / it->second.second;
    };
    auto barycentric = [&](int t, QPointF p, double l[3]) {
        const QPointF a = corner(t, 0), b = corner(t, 1), c = corner(t, 2);
        const double det = (b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y());
        l[1] = ((p.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (p.y() - a.y())) / det;
        l[2] = ((b.x() - a.x()) * (p.y() - a.y()) - (p.x() - a.x()) * (b.y() - a.y())) / det;
        l[0] = 1 - l[1] - l[2];
    };
    auto fieldAt = [&](int t, QPointF p) {
        double l[3];
        barycentric(t, p, l);
        QPointF E(0, 0);
        for (int k = 0; k < 3; ++k) E += std::clamp(l[k], 0.0, 1.0) * nodalIn(t, k);
        return E;
    };
    // The highest field where it ionizes: a node's, the line from it.
    int start = -1, startNode = -1;
    double Emax = 0;
    for (int t = 0; t < sp.elements(); ++t) {
        if (!ionizes(t)) continue;
        for (int k = 0; k < 3; ++k) {
            const QPointF E = nodalIn(t, k);
            const double e = std::hypot(E.x(), E.y());
            if (e > Emax) {
                Emax = e;
                start = t;
                startNode = k;
            }
        }
    }
    if (start < 0 || !(Emax > 0)) return;
    a_Emax = Emax;
    // The triangle holding p, walked to from t (across the edge p is beyond).
    auto locate = [&](int t, QPointF p) {
        for (int walk = 0; walk < 2000 && t >= 0; ++walk) {
            double l[3];
            barycentric(t, p, l);
            int worst = 0;
            for (int k = 1; k < 3; ++k)
                if (l[k] < l[worst]) worst = k;
            if (l[worst] >= -1e-12) return t;
            // (Beyond the edge opposite its corner 'worst': edge worst+1 to worst+2.)
            t = a_neighbors[std::size_t(t)][std::size_t((worst + 1) % 3)];
        }
        return -1;
    };
    // Along the field (sign 1) or against it, from that node (nudged into
    // its triangle): each step a fifth of its triangle's size, the field
    // where it is, until it leaves what ionizes, the field turns back or
    // falls to a thousandth of the highest.
    struct Sample {
        double s, E;
        int t;
    };
    const QPointF from = 0.98 * corner(start, startNode) + 0.01 * (corner(start, (startNode + 1) % 3) + corner(start, (startNode + 2) % 3));
    auto follow = [&](double sign) {
        std::vector<Sample> out;
        int t = start;
        QPointF p = from;
        double s = 0;
        for (int step = 0; step < 200000; ++step) {
            const QPointF E = fieldAt(t, p);
            const double e = std::hypot(E.x(), E.y());
            if (!(e > 1e-3 * Emax)) break;
            const QPointF a = corner(t, 0), b = corner(t, 1), c = corner(t, 2);
            const double size = std::sqrt(std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2);
            const QPointF d = sign * E / e;
            const QPointF q = p + d * (0.2 * size);
            const int next = locate(t, q);
            if (!ionizes(next)) break;
            const QPointF En = fieldAt(next, q);
            if ((En.x() * d.x() + En.y() * d.y()) * sign <= 0) break;   // (the field turned)
            s += 0.2 * size;
            out.push_back({s, std::hypot(En.x(), En.y()), next});
            p = q;
            t = next;
        }
        return out;
    };
    const std::vector<Sample> against = follow(-1), along = follow(1);
    // Ordered along the field (x: where holes go): against reversed, the
    // start, along.
    std::vector<Sample> line;
    for (auto it = against.rbegin(); it != against.rend(); ++it) line.push_back({-it->s, it->E, it->t});
    line.push_back({0, Emax, start});
    for (const Sample& sm : along) line.push_back(sm);
    if (line.size() < 2) return;
    const std::size_t n = line.size();
    std::vector<double> xs(n), an(n), ap(n);
    for (std::size_t k = 0; k < n; ++k) {
        xs[k] = line[k].s - line.front().s;
        const Medium& m = a_media[std::size_t(mesh.domain[std::size_t(line[k].t)])];
        double d = 0;
        ionizationRate(line[k].E, m.an, m.bn, m.an2, m.bn2, m.E0, m.ionBetaN, &an[k], &d);
        ionizationRate(line[k].E, m.ap, m.bp, m.ap2, m.bp2, m.E0, m.ionBetaP, &ap[k], &d);
    }
    // Holes along x from 0: Ip = ∫ αp e^(-∫_0^x (αp - αn)) dx; electrons
    // against it from W: In = ∫ αn e^(-∫_x^W (αn - αp)) dx.
    std::vector<double> K(n, 0.0);   // ∫_0^x (αp - αn)
    for (std::size_t k = 1; k < n; ++k) K[k] = K[k - 1] + 0.5 * ((ap[k] - an[k]) + (ap[k - 1] - an[k - 1])) * (xs[k] - xs[k - 1]);
    double Ip = 0, In = 0;
    for (std::size_t k = 1; k < n; ++k) {
        const double dx = xs[k] - xs[k - 1];
        Ip += 0.5 * (ap[k] * std::exp(-K[k]) + ap[k - 1] * std::exp(-K[k - 1])) * dx;
        // ∫_x^W (αn - αp) = K(x) - K(W)... (αn - αp) = -(αp - αn): -(K(W) - K(x)).
        In += 0.5 * (an[k] * std::exp(K[n - 1] - K[k]) + an[k - 1] * std::exp(K[n - 1] - K[k - 1])) * dx;
    }
    a_In = In;
    a_Ip = Ip;
    a_ionLength = xs.back();
}

bool SemiconductorProblem::stationary(QString* why)
{
    Field& f = field();
    std::vector<double> goal;
    if (!targets(goal, why)) return false;
    Eigen::VectorXd x = stateVector();
    auto finish = [&](const Eigen::VectorXd& solved, bool still) {
        for (std::size_t k = 0; k < a_contacts.size(); ++k) a_contacts[k].applied = goal[k];
        terminalValues(solved, 0, nullptr, still);
        store(solved);
    };
    double fermi = 0;
    if (atEquilibrium(goal, &fermi)) {
        // No current: Poisson's equation alone (a MOS capacitor, a junction
        // unbiased) - exact, the currents' equations hold at once.
        if (!equilibriumPoisson(x, fermi, goal, f.solved, why)) return false;
        finish(x, true);
        quasiStatic(x, fermi, goal);
        return true;
    }
    a_C0.clear();
    std::vector<double> at(a_contacts.size(), 0.0);
    if (!f.solved) {
        if (!equilibriumPoisson(x, 0, at, false, why)) return false;
        for (Contact& c : a_contacts) c.applied = 0;
    } else {
        for (std::size_t k = 0; k < a_contacts.size(); ++k) at[k] = a_contacts[k].applied;
    }
    // From the voltages it is at to those asked, in steps that Newton's
    // method takes (smaller when it does not).
    double step = 0.1;
    int solves = 0;
    QString failed;
    while (true) {
        double left = 0;
        for (std::size_t k = 0; k < goal.size(); ++k) left = std::max(left, std::abs(goal[k] - at[k]));
        if (left < 1e-12 && solves > 0) break;
        const double frac = left > 0 ? std::min(1.0, step / left) : 1.0;
        std::vector<double> next(at.size());
        for (std::size_t k = 0; k < at.size(); ++k) next[k] = at[k] + frac * (goal[k] - at[k]);
        contactValues(next);
        Eigen::VectorXd trial = x;
        int steps = 0;
        if (newton(trial, 0, nullptr, 40, &steps, &failed)) {
            x = trial;
            at = next;
            ++solves;
            if (steps < 8) step = std::min(step * 2, 1.0);
        } else {
            step /= 4;
            if (step < 1e-5) {
                // (Not "did not converge", the cause's words too: a step at
                // rest that fails is an error - said as a warning, its study
                // went on with nothing solved, its currents nought.)
                QString cause = failed.section(QLatin1String(": "), 1);
                cause.replace(QLatin1String("did not converge"), QLatin1String("failed to converge"));
                *why = tr("%1: no solution found on the way to the contacts' voltages (steps down to 10 µV): %2").arg(a_node->name(), cause);
                return false;
            }
        }
    }
    finish(x, false);
    if (solves > 1) a_s.log << tr("%1: the contacts' voltages reached in %2 steps").arg(a_node->name()).arg(solves);
    return true;
}

bool SemiconductorProblem::smallSignal(double frequency, QString* why)
{
    const Eigen::VectorXd x = stateVector();
    std::vector<double> at(a_contacts.size());
    for (std::size_t k = 0; k < a_contacts.size(); ++k) at[k] = a_contacts[k].applied;
    contactValues(at);
    updateMobilities(x);
    Eigen::VectorXd F;
    std::vector<Eigen::Triplet<double>> trip;
    assemble(x, 0, nullptr, F, &trip);
    std::vector<char> fixed(std::size_t(a_unknowns), 0);
    for (const Contact& c : a_contacts)
        for (int i : c.nodes) {
            fixed[std::size_t(a_iV[std::size_t(i)])] = 1;
            if (c.kind == Contact::Ohmic) fixed[std::size_t(a_iN[std::size_t(i)])] = fixed[std::size_t(a_iP[std::size_t(i)])] = 1;
        }
    // J + jω M (M the carriers' boxes: -qA on n's rows, +qA on p's), solved in
    // the quasi-Fermi potentials' changes: δn = n/Vt (δV - δφn), δp = p/Vt
    // (δφp - δV) (Fermi-Dirac's n ℱ'/ℱ, p ℱ'/ℱ for n, p). In n and p a current near nought is the difference of two
    // large terms (a MOS capacitor's inversion electrons, 10^14 above the
    // bulk's): in φ its gradient, well conditioned.
    const double omega = 2 * M_PI * frequency;
    using cd = std::complex<double>;
    std::vector<int> colNode(std::size_t(a_unknowns), -1), colKind(std::size_t(a_unknowns), 0);   // 0 V, 1 n, 2 p
    for (int i = 0; i < a_n; ++i) {
        if (a_iV[std::size_t(i)] >= 0) colNode[std::size_t(a_iV[std::size_t(i)])] = i;
        if (a_iN[std::size_t(i)] >= 0) {
            colNode[std::size_t(a_iN[std::size_t(i)])] = i;
            colKind[std::size_t(a_iN[std::size_t(i)])] = 1;
            colNode[std::size_t(a_iP[std::size_t(i)])] = i;
            colKind[std::size_t(a_iP[std::size_t(i)])] = 2;
        }
    }
    std::vector<Eigen::Triplet<cd>> ct;
    // An entry of the n-p system in the φ one's columns (each in units of Vt).
    auto put = [&](int r, int c, cd v) {
        const int i = colNode[std::size_t(c)];
        if (colKind[std::size_t(c)] == 0) ct.emplace_back(r, c, v * a_Vt);
        else if (colKind[std::size_t(c)] == 1) {
            // (n/Vt, times Vt; Fermi-Dirac's n ℱ'/ℱ.)
            const double s = x(c) * a_rN[std::size_t(i)];
            ct.emplace_back(r, a_iV[std::size_t(i)], v * s);
            ct.emplace_back(r, c, -v * s);
        } else {
            const double s = x(c) * a_rP[std::size_t(i)];
            ct.emplace_back(r, a_iV[std::size_t(i)], -v * s);
            ct.emplace_back(r, c, v * s);
        }
    };
    for (const auto& t : trip)
        if (!fixed[std::size_t(t.row())]) put(t.row(), t.col(), cd(t.value(), 0));
    // Interface traps follow with their own time: δf = (cn (1 - f) δn -
    // cp f δp)/(jω + 1/τ), 1/τ = cn (n + n1) + cp (p + p1) - each level's
    // charge -q N δf, the electrons it takes δUn = N (cn (1 - f) δn - cn (n
    // + n1) δf), the holes δUp = N (cp f δp + cp (p + p1) δf): at jω, in
    // the place of the static changes assembled (to which they come at 0).
    auto trapChanges = [&](const TrapLevel& tl, const TrapState& st, double n, double p, cd* dsn, cd* dsp, cd* dun, cd* dup, cd* dvn, cd* dvp) {
        const cd denom(st.D, omega);
        const cd fn = tl.cn * (1 - st.f) / denom, fp = -tl.cp * st.f / denom;   // δf per δn, δp
        *dsn = -Q * tl.N * fn;
        *dsp = -Q * tl.N * fp;
        *dun = tl.N * (tl.cn * (1 - st.f) - tl.cn * (n + tl.n1) * fn);
        *dup = tl.N * (-tl.cn * (n + tl.n1) * fp);
        *dvn = tl.N * (tl.cp * (p + tl.p1) * fn);
        *dvp = tl.N * (tl.cp * st.f + tl.cp * (p + tl.p1) * fp);
    };
    for (const TrapLevel& tl : a_traps) {
        const std::size_t k = std::size_t(tl.node);
        const int rv = a_iV[k], rn = a_iN[k], rp = a_iP[k];
        const double gn = std::exp(a_lgN[k]), gp = std::exp(a_lgP[k]);
        const double n = x(rn) / gn, p = x(rp) / gp;
        const TrapState st = trapState(tl, n, p);
        cd dsn, dsp, dun, dup, dvn, dvp;
        trapChanges(tl, st, n, p, &dsn, &dsp, &dun, &dup, &dvn, &dvp);
        if (!fixed[std::size_t(rv)]) {
            put(rv, rn, (dsn + Q * tl.N * st.dfdn) * tl.area / gn);
            put(rv, rp, (dsp + Q * tl.N * st.dfdp) * tl.area / gp);
        }
        if (!fixed[std::size_t(rn)]) {
            put(rn, rn, -Q * tl.area * (dun - st.dUdn) / gn);
            put(rn, rp, -Q * tl.area * (dup - st.dUdp) / gp);
        }
        if (!fixed[std::size_t(rp)]) {
            put(rp, rn, Q * tl.area * (dvn - st.dUdn) / gn);
            put(rp, rp, Q * tl.area * (dvp - st.dUdp) / gp);
        }
    }
    for (int i = 0; i < a_n; ++i) {
        const double A = a_box[std::size_t(i)];
        const int rn = a_iN[std::size_t(i)], rp = a_iP[std::size_t(i)];
        if (rn >= 0 && !fixed[std::size_t(rn)]) put(rn, rn, cd(0, -omega * Q * A));
        if (rp >= 0 && !fixed[std::size_t(rp)]) put(rp, rp, cd(0, omega * Q * A));
    }
    // Fixed: δV at a contact; δφn = δφp = δVa at an ohmic one (its carriers
    // fixed).
    for (int k = 0; k < a_unknowns; ++k)
        if (fixed[std::size_t(k)]) ct.emplace_back(k, k, cd(a_Vt, 0));
    Eigen::SparseMatrix<cd> A(a_unknowns, a_unknowns);
    A.setFromTriplets(ct.begin(), ct.end());
    Eigen::VectorXd rowMax = Eigen::VectorXd::Zero(a_unknowns);
    for (int k = 0; k < A.outerSize(); ++k)
        for (Eigen::SparseMatrix<cd>::InnerIterator itr(A, k); itr; ++itr) rowMax(itr.row()) = std::max(rowMax(itr.row()), std::abs(itr.value()));
    for (int k = 0; k < a_unknowns; ++k)
        if (!(rowMax(k) > 0)) rowMax(k) = 1;
    for (int k = 0; k < A.outerSize(); ++k)
        for (Eigen::SparseMatrix<cd>::InnerIterator itr(A, k); itr; ++itr) itr.valueRef() /= rowMax(itr.row());
    A.makeCompressed();
    Eigen::SparseLU<Eigen::SparseMatrix<cd>> lu;
    lu.analyzePattern(A);
    lu.factorize(A);
    if (lu.info() != Eigen::Success) {
        *why = tr("%1: the small-signal matrix is singular").arg(a_node->name());
        return false;
    }
    // The reactions' rows (each contact node's V row as assembled: its
    // charge); the currents weighed as at rest (currentWeights()).
    std::map<int, std::vector<std::pair<int, double>>> rows;
    for (const auto& t : trip)
        if (fixed[std::size_t(t.row())] && colNode[std::size_t(t.row())] >= 0 && colKind[std::size_t(t.row())] == 0) rows[t.row()].emplace_back(t.col(), t.value());
    const std::size_t nc = a_contacts.size();
    const std::vector<std::vector<double>> weight = currentWeights(x);
    // Each contact's current's change for a solution's change dy (δV/Vt,
    // δφn/Vt, δφp/Vt): the edges' currents' and the carriers' at jω weighed,
    // and its metal's charge's at jω.
    auto terminals = [&](const Eigen::VectorXcd& dy) {
        Eigen::VectorXcd dx(a_unknowns);
        for (int j = 0; j < a_unknowns; ++j) {
            const int i = colNode[std::size_t(j)];
            // (As the system's columns have it: Fermi-Dirac's n ℱ'/ℱ.)
            if (colKind[std::size_t(j)] == 0) dx(j) = dy(j) * a_Vt;
            else if (colKind[std::size_t(j)] == 1) dx(j) = x(j) * a_rN[std::size_t(i)] * (dy(a_iV[std::size_t(i)]) - dy(j));
            else dx(j) = x(j) * a_rP[std::size_t(i)] * (dy(j) - dy(a_iV[std::size_t(i)]));
        }
        auto rowDot = [&](int r) {
            cd s(0, 0);
            auto it = rows.find(r);
            if (it == rows.end()) return s;
            for (const auto& [c, v] : it->second) s += v * dx(c);
            return s;
        };
        // Each edge's change of current (Scharfetter and Gummel's, linear).
        std::vector<cd> df(a_edges.size(), cd(0, 0));
        for (std::size_t m = 0; m < a_edges.size(); ++m) {
            const Edge& e = a_edges[m];
            if (e.w <= 0) continue;
            const int i = e.i, j = e.j;
            double dN = 0, dP = 0;
            edgeSteps(e, x, &dN, &dP);
            const int ni = a_iN[std::size_t(i)], nj = a_iN[std::size_t(j)], pi = a_iP[std::size_t(i)], pj = a_iP[std::size_t(j)];
            const cd dV = (dx(a_iV[std::size_t(j)]) - dx(a_iV[std::size_t(i)])) / a_Vt;
            // (Each step's change: the potential's, and Fermi-Dirac's ln γ at
            // each end with its carriers.)
            const cd dVn = dV + a_dlgN[std::size_t(j)] * dx(nj) - a_dlgN[std::size_t(i)] * dx(ni);
            const cd dVp = dV - a_dlgP[std::size_t(j)] * dx(pj) + a_dlgP[std::size_t(i)] * dx(pi);
            const cd dfn = e.wn * (bern(dN) * dx(nj) - bern(-dN) * dx(ni) + (x(nj) * dbern(dN) + x(ni) * dbern(-dN)) * dVn);
            const cd dfp = e.wp * (bern(dP) * dx(pi) - bern(-dP) * dx(pj) + (x(pi) * dbern(dP) + x(pj) * dbern(-dP)) * dVp);
            df[m] = dfn + dfp;
        }
        std::vector<cd> out(nc, cd(0, 0));
        for (std::size_t c = 0; c < nc; ++c) {
            const Contact& ct2 = a_contacts[c];
            cd dI(0, 0), dQ(0, 0);
            for (int i : ct2.nodes) dQ -= rowDot(a_iV[std::size_t(i)]);
            const std::vector<double>& w = weight[c];
            if (!w.empty()) {
                for (std::size_t m = 0; m < a_edges.size(); ++m) {
                    const double dw = w[std::size_t(a_edges[m].i)] - w[std::size_t(a_edges[m].j)];
                    if (dw != 0) dI += df[m] * dw;
                }
                // (The carriers' change, -qA δn and qA δp on their rows, at jω;
                // the interface traps' charge's.)
                for (int i = 0; i < a_n; ++i)
                    if (w[std::size_t(i)] != 0 && a_iN[std::size_t(i)] >= 0)
                        dI += w[std::size_t(i)] * cd(0, omega) * Q * a_box[std::size_t(i)] * (dx(a_iP[std::size_t(i)]) - dx(a_iN[std::size_t(i)]));
                for (const TrapLevel& tl : a_traps) {
                    const std::size_t k = std::size_t(tl.node);
                    if (w[k] == 0) continue;
                    const double gn = std::exp(a_lgN[k]), gp = std::exp(a_lgP[k]);
                    const double n = x(a_iN[k]) / gn, p = x(a_iP[k]) / gp;
                    cd dsn, dsp, dun, dup, dvn, dvp;
                    trapChanges(tl, trapState(tl, n, p), n, p, &dsn, &dsp, &dun, &dup, &dvn, &dvp);
                    dI += w[k] * cd(0, omega) * tl.area * (dsn * dx(a_iN[k]) / gn + dsp * dx(a_iP[k]) / gp);
                }
            }
            out[c] = dI + cd(0, omega) * dQ;
        }
        return out;
    };
    a_Y.assign(nc, std::vector<cd>(nc, cd(0, 0)));
    for (std::size_t k = 0; k < nc; ++k) {
        Eigen::VectorXcd rhs = Eigen::VectorXcd::Zero(a_unknowns);
        // (A fixed row reads Vt y = 1 V: y in units of Vt.)
        for (int i : a_contacts[k].nodes) {
            rhs(a_iV[std::size_t(i)]) = cd(1.0 / rowMax(a_iV[std::size_t(i)]), 0);
            if (a_contacts[k].kind == Contact::Ohmic) {
                rhs(a_iN[std::size_t(i)]) = cd(1.0 / rowMax(a_iN[std::size_t(i)]), 0);
                rhs(a_iP[std::size_t(i)]) = cd(1.0 / rowMax(a_iP[std::size_t(i)]), 0);
            }
        }
        const std::vector<cd> dI = terminals(lu.solve(rhs));
        for (std::size_t c = 0; c < nc; ++c) a_Y[c][k] = dI[c];
    }
    // The light modulated wholly (δG = G), the contacts held: each one's
    // current - a photodiode's response, its carriers' transit.
    a_Hop.clear();
    if (a_Gop > 0) {
        Eigen::VectorXcd rhs = Eigen::VectorXcd::Zero(a_unknowns);
        for (int i = 0; i < a_n; ++i) {
            const double g = a_generation.empty() ? 0.0 : a_generation[std::size_t(i)];
            if (g == 0 || a_iN[std::size_t(i)] < 0) continue;
            const int rn = a_iN[std::size_t(i)], rp = a_iP[std::size_t(i)];
            // (F's rows hold -qA (R - G) and qA (R - G): ∂F/∂G qA and -qA; J δx = -∂F/∂G δG.)
            if (!fixed[std::size_t(rn)]) rhs(rn) = cd(-Q * a_box[std::size_t(i)] * g / rowMax(rn), 0);
            if (!fixed[std::size_t(rp)]) rhs(rp) = cd(Q * a_box[std::size_t(i)] * g / rowMax(rp), 0);
        }
        a_Hop = terminals(lu.solve(rhs));
    }
    a_frequency = frequency;
    return true;
}

void SemiconductorProblem::initialize()
{
    // From where it is (a step before's), or a solution at rest at the
    // contacts' voltages now: a study in time starts there.
    if (field().solved) return;
    a_generation.assign(std::size_t(a_n), 0.0);
    QString why;
    if (!stationary(&why)) a_initError = why;
}

void SemiconductorProblem::latticeTemperature()
{
    if (!a_TL.isValid()) return;
    // Each triangle's lattice temperature at its middle: its mobilities'
    // factors (TL/300 K)^-ζ, the edges weighed again.
    PointEvaluator ev(a_s);
    for (int t = 0; t < int(a_elements.size()); ++t) {
        ElementMobility& em = a_elements[std::size_t(t)];
        if (em.domain < 0) continue;
        ev.moveTo(t, 1.0 / 3, 1.0 / 3);
        const double T = ev.eval(a_TL);
        if (!(T > 0)) continue;
        const Medium& m = a_media[std::size_t(em.domain)];
        em.tn = std::pow(T / 300, -m.zetan);
        em.tp = std::pow(T / 300, -m.zetap);
    }
    edgeWeights(nullptr);
}

bool SemiconductorProblem::solve(const SolveOptions& o, QString* error)
{
    if (!a_initError.isEmpty()) {
        *error = a_initError;
        return false;
    }
    latticeTemperature();
    // Generation at the nodes, as it is now (it may read other fields, t).
    a_generation.assign(std::size_t(a_n), 0.0);
    {
        // The light of each mode fed P0: its power absorbed across the gap
        // (Qgap, for 1 W) made pairs, a photon each, η of them - along the
        // device its power falls as e^(-2αz): over its length L (the
        // thickness) the mean (1 - e^(-2αL))/2αL of what it starts with. A
        // mode not solved yet (the dark current's step): none.
        std::vector<double> perWatt(a_optical.size(), 0.0);
        a_Pin = 0;
        for (std::size_t k = 0; k < a_optical.size(); ++k) {
            OpticalSource& o = a_optical[k];
            const std::optional<double> alpha = a_s.global(o.mode + QStringLiteral(".alpha"));
            const std::optional<double> lambda = a_s.global(o.mode + QStringLiteral(".lambda0"));
            if (!alpha || !lambda || !(*lambda > 0)) continue;
            // The mode it asks for: the one its step left selected (else
            // said - the light another mode's).
            const std::optional<double> selected = a_s.global(o.mode + QStringLiteral(".mode"));
            if (selected && int(std::lround(*selected)) != o.number) {
                *error = tr("%1: its Optical Generation is lit by mode %2 of %3, and mode %4 is the one solved last (a mode step "
                            "finds as many modes as it asks for: at least %2)")
                             .arg(a_node->name())
                             .arg(o.number)
                             .arg(o.mode)
                             .arg(int(std::lround(*selected)));
                return false;
            }
            // The power fed now (of t in time).
            if (o.power.isValid() && !o.power.isConstant()) {
                PointEvaluator pe(a_s);
                const auto& [t0, r0] = a_nodePoint[std::size_t(std::find_if(a_nodeDomain.begin(), a_nodeDomain.end(), [](int d) { return d >= 0; }) - a_nodeDomain.begin())];
                pe.moveTo(t0, r0[0], r0[1]);
                o.P0 = pe.eval(o.power);
                if (!(std::isfinite(o.P0) && o.P0 >= 0)) {
                    *error = tr("%1: the power fed its Optical Generation is %2, not a number of 0 or more").arg(a_node->name()).arg(o.P0);
                    return false;
                }
            }
            const double L = a_s.axisymmetric() ? 1.0 : a_s.thickness(), x = 2 * std::max(*alpha, 0.0) * L;
            const double mean = x > 1e-9 ? -std::expm1(-x) / x : 1.0;
            const double photon = 6.62607015e-34 * 299792458.0 / *lambda;
            perWatt[k] = o.eta * o.P0 * mean / photon;
            a_Pin += o.P0;
        }
        // By triangle, each its own medium's at its corners, a third of its
        // volume to each (a node's box spans the media about it: a node on
        // germanium's edge took the light germanium absorbs over its
        // silicon's share too - more pairs than photons). Each mode's light
        // then scaled to what the mode itself absorbs over those triangles
        // (its own rule): a junction's corners do not take more or less.
        const Space& sp = *field().space;
        const Mesh& mesh = a_s.mesh();
        const double unit = mesh.unitScale;
        static const double ref[3][2] = {{0, 0}, {1, 0}, {0, 1}};
        PointEvaluator ev(a_s);
        std::vector<double> amount(std::size_t(a_n), 0.0);
        std::vector<std::vector<double>> lit(a_optical.size());   // by source: its pairs by node, before its scale
        std::vector<double> exact(a_optical.size(), 0.0), discrete(a_optical.size(), 0.0);
        const TriangleRule& rule = triangleRule(4);
        for (int t = 0; t < sp.elements(); ++t) {
            if (!a_elemSemi[std::size_t(t)]) continue;
            const int d = mesh.domain[std::size_t(t)];
            const Medium& m = a_media[std::size_t(d)];
            if (m.generation.empty() && m.optical.empty()) continue;
            const int* dof = sp.dofs(t);
            QPointF x[3];
            for (int k = 0; k < 3; ++k) x[k] = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * unit;
            const double det = (x[1].x() - x[0].x()) * (x[2].y() - x[0].y()) - (x[2].x() - x[0].x()) * (x[1].y() - x[0].y());
            const double third = std::abs(det) / 2 * a_s.weight((x[0] + x[1] + x[2]) / 3.0) / 3;
            for (int k = 0; k < 3; ++k) {
                ev.moveTo(t, ref[k][0], ref[k][1]);
                double g = 0;
                for (const Expression& e : m.generation) {
                    const double v = ev.eval(e);
                    if (std::isfinite(v)) g += v;
                }
                amount[std::size_t(dof[k])] += g * third;
                for (int o : m.optical) {
                    if (perWatt[std::size_t(o)] == 0 || !a_optical[std::size_t(o)].absorbed.isValid()) continue;
                    const double q = ev.eval(a_optical[std::size_t(o)].absorbed);
                    if (!(std::isfinite(q) && q > 0)) continue;
                    std::vector<double>& to = lit[std::size_t(o)];
                    if (to.empty()) to.assign(std::size_t(a_n), 0.0);
                    to[std::size_t(dof[k])] += q * third;
                    discrete[std::size_t(o)] += q * third;
                }
            }
            for (int o : m.optical) {
                if (perWatt[std::size_t(o)] == 0 || !a_optical[std::size_t(o)].absorbed.isValid()) continue;
                for (const auto& q : rule.points) {
                    ev.moveTo(t, q[0], q[1]);
                    const QPointF at = x[0] + q[0] * (x[1] - x[0]) + q[1] * (x[2] - x[0]);
                    const double v = ev.eval(a_optical[std::size_t(o)].absorbed);
                    if (std::isfinite(v) && v > 0) exact[std::size_t(o)] += q[2] * std::abs(det) * a_s.weight(at) * v;
                }
            }
        }
        for (std::size_t o = 0; o < a_optical.size(); ++o) {
            if (lit[o].empty() || !(discrete[o] > 0)) continue;
            const double scale = perWatt[o] * exact[o] / discrete[o];
            for (int i = 0; i < a_n; ++i) amount[std::size_t(i)] += lit[o][std::size_t(i)] * scale;
        }
        for (int i = 0; i < a_n; ++i)
            if (a_box[std::size_t(i)] > 0) a_generation[std::size_t(i)] = amount[std::size_t(i)] / a_box[std::size_t(i)];
        a_Gop = 0;
        for (int i = 0; i < a_n; ++i) a_Gop += a_generation[std::size_t(i)] * a_box[std::size_t(i)];
    }
    if (o.harmonic && o.frequency > 0) {
        if (!field().solved && !stationary(error)) return false;
        return smallSignal(o.frequency, error);
    }
    a_Y.clear();
    if (o.mass > 0 && o.history) {
        // A step in time: straight to the contacts' voltages now.
        std::vector<double> goal;
        if (!targets(goal, error)) return false;
        contactValues(goal);
        Eigen::VectorXd x = stateVector();
        int steps = 0;
        QString why;
        if (!newton(x, o.mass, o.history, 40, &steps, &why)) {
            *error = why;   // ("did not converge": the step is taken again, smaller)
            return false;
        }
        for (std::size_t k = 0; k < a_contacts.size(); ++k) a_contacts[k].applied = goal[k];
        terminalValues(x, o.mass, o.history);
        // (Displacement current: the contact's charge's change.)
        {
            // Q at the step's start from the history's charge: the BDF
            // formula applied to Q (affine in the unknowns) is mass Q(x) - Q(history).
            std::vector<double> hv = *o.history;
            Eigen::VectorXd xh = Eigen::VectorXd::Zero(a_unknowns);
            const std::size_t N = std::size_t(a_n);
            for (int i = 0; i < a_n; ++i) {
                if (a_iV[std::size_t(i)] >= 0) xh(a_iV[std::size_t(i)]) = hv[std::size_t(i)];
                if (a_iN[std::size_t(i)] >= 0) xh(a_iN[std::size_t(i)]) = hv[N + std::size_t(i)];
                if (a_iP[std::size_t(i)] >= 0) xh(a_iP[std::size_t(i)]) = hv[2 * N + std::size_t(i)];
            }
            // The charges' linear parts: Poisson's rows less the doping's.
            auto charges = [&](const Eigen::VectorXd& v, bool withDoping) {
                Eigen::VectorXd F;
                assemble(v, 0, nullptr, F, nullptr);
                std::vector<double> q(a_contacts.size(), 0);
                for (std::size_t k = 0; k < a_contacts.size(); ++k)
                    for (int i : a_contacts[k].nodes) {
                        double fv = F(a_iV[std::size_t(i)]);
                        if (!withDoping && a_nodeDomain[std::size_t(i)] >= 0) fv -= Q * a_box[std::size_t(i)] * doping(i);
                        q[k] -= fv;
                    }
                return q;
            };
            const std::vector<double> now = charges(x, false), before = charges(xh, false);
            for (std::size_t k = 0; k < a_contacts.size(); ++k) a_contacts[k].current += o.mass * now[k] - before[k];
        }
        store(x);
        return true;
    }
    return stationary(error);
}

QString matrixEntryName(const QString& tag, const QString& letter, const QString& a, const QString& b)
{
    if (a.size() == 1 && b.size() == 1) return tag + QLatin1Char('.') + letter + a + b;
    return tag + QLatin1Char('.') + letter + QLatin1Char('_') + a + QLatin1Char('_') + b;
}

void SemiconductorProblem::setGlobals()
{
    const QString p = field().tag + QLatin1Char('.');
    if (a_ionizationAny && field().solved) {
        ionizationIntegrals(stateVector());
        const Dim field = Dim::of(1, 1, -3, -1);
        a_s.setGlobal(p + QStringLiteral("In"), a_In, Dim(),
                      tr("electrons' ionization integral along the field line through the highest field: breakdown where it reaches 1"));
        a_s.setGlobal(p + QStringLiteral("Ip"), a_Ip, Dim(), tr("holes' ionization integral along that line: breakdown where it reaches 1"));
        a_s.setGlobal(p + QStringLiteral("Mn"), a_In < 1 ? 1 / (1 - a_In) : Inf, Dim(),
                      tr("multiplication of electrons entering the line at its end, 1/(1 - In) (infinite: past breakdown)"));
        a_s.setGlobal(p + QStringLiteral("Mp"), a_Ip < 1 ? 1 / (1 - a_Ip) : Inf, Dim(),
                      tr("multiplication of holes entering the line at its start, 1/(1 - Ip)"));
        a_s.setGlobal(p + QStringLiteral("Emax"), a_Emax, field, tr("the highest field where it ionizes"));
        a_s.setGlobal(p + QStringLiteral("Lion"), a_ionLength, Dim::length(), tr("the field line's length, where the integrals are taken"));
    }
    for (const Contact& c : a_contacts) {
        a_s.setGlobal(p + QStringLiteral("I_") + c.name, c.current, Dim::current(), tr("current into the device at %1").arg(c.name));
        a_s.setGlobal(p + QStringLiteral("V_") + c.name, c.applied, Dim::voltage(), tr("voltage applied at %1").arg(c.name));
        // (An ohmic contact on neutral silicon holds almost none: the
        // junction's charge is balanced inside it.)
        a_s.setGlobal(p + QStringLiteral("Q_") + c.name, c.charge, Dim::charge(),
                      c.kind == Contact::Ohmic ? tr("charge on %1's metal - about nought on neutral silicon (a junction's charge is balanced "
                                                    "inside it); its capacitance is the small-signal %2")
                                                     .arg(c.name, matrixEntryName(field().tag, QStringLiteral("C"), c.name, c.name))
                                               : tr("charge on %1's metal").arg(c.name));
    }
    for (std::size_t a = 0; a < a_C0.size(); ++a)
        for (std::size_t b = 0; b < a_C0.size(); ++b) {
            const QString na = a_contacts[std::size_t(a_gates[a])].name, nb = a_contacts[std::size_t(a_gates[b])].name;
            a_s.setGlobal(matrixEntryName(field().tag, QStringLiteral("C0"), na, nb), a_C0[a][b], Dim::capacitance(),
                          tr("quasi-static capacitance at equilibrium (the carriers following: low frequency), %1 from %2").arg(na, nb));
        }
    if (!a_Y.empty() && a_frequency > 0) {
        const double omega = 2 * M_PI * a_frequency;
        for (std::size_t a = 0; a < a_contacts.size(); ++a)
            for (std::size_t b = 0; b < a_contacts.size(); ++b) {
                const QString na = a_contacts[a].name, nb = a_contacts[b].name;
                a_s.setGlobal(matrixEntryName(field().tag, QStringLiteral("G"), na, nb), a_Y[a][b].real(), Dim::conductance(),
                              tr("small-signal conductance, %1 from %2").arg(na, nb));
                a_s.setGlobal(matrixEntryName(field().tag, QStringLiteral("C"), na, nb), a_Y[a][b].imag() / omega, Dim::capacitance(),
                              tr("small-signal capacitance, %1 from %2").arg(na, nb));
            }
    }
    // Light: the pairs it makes, what they would carry each collected, the
    // power fed; modulated, each contact's share of it - the carriers'
    // transit, and with the load RL across the contact its RC too (the
    // device's own admittance Y beside RL: H RL⁻¹/(RL⁻¹ + Y)).
    if (!a_optical.empty() || a_Gop > 0) {
        a_s.setGlobal(p + QStringLiteral("Gop"), a_Gop, Dim::of(0, 0, -1),
                      tr("electron-hole pairs made per second - the light absorbed across the gap, and any generation given"));
        a_s.setGlobal(p + QStringLiteral("Iop"), Q * a_Gop, Dim::current(), tr("their current, every pair collected: q Gop"));
    }
    if (!a_optical.empty())
        a_s.setGlobal(p + QStringLiteral("Pin"), a_Pin, Dim::power(),
                      tr("the light fed (W) - a responsivity |I|/Pin, an external quantum efficiency |I|/Pin hc/qλ"));
    if (!a_Hop.empty() && a_frequency > 0 && a_Gop > 0) {
        const double RL = a_optical.empty() ? 50.0 : a_optical.front().RL;
        for (std::size_t c = 0; c < a_contacts.size(); ++c) {
            const QString name = a_contacts[c].name;
            const std::complex<double> h = a_Hop[c] / (Q * a_Gop);
            a_s.setGlobal(p + QStringLiteral("Hop_") + name, std::abs(h), Dim(),
                          tr("small-signal photocurrent at %1 per pair made, the light modulated (1 at low frequency, every pair "
                             "collected): its transit's roll-off")
                              .arg(name));
            if (RL > 0 && a_Y.size() > c) {
                const std::complex<double> yl = 1 / RL, loaded = h * yl / (yl + a_Y[c][c]);
                a_s.setGlobal(p + QStringLiteral("HopRL_") + name, std::abs(loaded), Dim(),
                              tr("the same through the load RL across %1 (the device's capacitance charged through it): a photodiode's "
                                 "bandwidth, where it is 1/√2 of its low frequency's")
                                  .arg(name));
            }
        }
    }
}

} // namespace

std::unique_ptr<Problem> makeSemiconductorProblem(Solution& solution, int field)
{
    return std::make_unique<SemiconductorProblem>(solution, field);
}

SemiconductorState semiconductorAt(PointEvaluator& e, int f)
{
    SemiconductorState s;
    const Solution& sol = e.solution();
    const Field& field = sol.fields()[std::size_t(f)];
    const int t = e.triangle();
    if (t < 0 || field.extra.size() < std::size_t(ExtraHead + PerTriangle * (t + 1))) return s;
    const double* ex = &field.extra[std::size_t(ExtraHead + PerTriangle * t)];
    const double Vt = field.extra[0], niRef = field.extra[2];
    const int d = sol.mesh().domain[std::size_t(t)];
    if (d < 0 || !field.domains[std::size_t(d)]) return s;
    const Space& sp = *field.space;
    const int* dof = sp.dofs(t);
    const std::size_t N = std::size_t(sp.size());
    const double xi = e.xi(), eta = e.eta();
    const double w[3] = {1 - xi - eta, xi, eta};
    double V[3], n[3], p[3], thn[3], thp[3];
    for (int k = 0; k < 3; ++k) {
        V[k] = field.values[std::size_t(dof[k])];
        n[k] = field.values[N + std::size_t(dof[k])];
        p[k] = field.values[2 * N + std::size_t(dof[k])];
        const bool banded = field.values.size() >= 7 * N;
        thn[k] = banded ? field.values[5 * N + std::size_t(dof[k])] : 0.0;
        thp[k] = banded ? field.values[6 * N + std::size_t(dof[k])] : 0.0;
    }
    s.V = w[0] * V[0] + w[1] * V[1] + w[2] * V[2];
    s.defined = true;
    if (!std::isfinite(ex[0])) return s;   // an insulator
    s.semiconductor = true;
    s.Vt = Vt;
    // Carriers interpolated in their logarithm: they vary exponentially.
    double ln = 0, lp = 0;
    for (int k = 0; k < 3; ++k) {
        ln += w[k] * std::log(std::max(n[k], 1e-300));
        lp += w[k] * std::log(std::max(p[k], 1e-300));
    }
    s.n = std::exp(ln);
    s.p = std::exp(lp);
    s.mun = ex[0];
    s.mup = ex[1];
    s.dEc = ex[2];
    s.Eg = ex[3];
    s.ni = ex[4];
    s.delta = std::isfinite(ex[11]) ? ex[11] : 0.0;
    if (std::isfinite(ex[12]) && std::isfinite(ex[14])) {
        s.n1d = ex[12];
        s.gd = ex[13];
        s.p1a = ex[14];
        s.ga = ex[15];
    }
    // Fermi-Dirac's: γ = ℱ(η)/e^η, the carriers' Boltzmann densities n/γ.
    double gn = 1, gp = 1;
    s.joule = ex[18];
    if (ex[16] > 0 && ex[17] > 0) {
        const FermiHalf& fh = fermiHalf();
        auto gamma = [&](double c, double N) {
            const double u = c / N;
            return u > 1e-6 ? u / std::exp(fh.inverse(u)) : 1 - u / std::sqrt(8.0);
        };
        gn = gamma(s.n, ex[16]);
        gp = gamma(s.p, ex[17]);
    }
    s.nB = s.n / gn;
    s.pB = s.p / gp;
    // The quasi-Fermi potentials at the nodes (each its own bands'), linear
    // between them: φn = V + θn - Vt ln(n/ni_ref), φp = V + θp + Vt ln(p/ni_ref).
    s.phin = s.phip = 0;
    for (int k = 0; k < 3; ++k) {
        s.phin += w[k] * (V[k] + thn[k] - Vt * std::log(std::max(n[k], 1e-300) / niRef));
        s.phip += w[k] * (V[k] + thp[k] + Vt * std::log(std::max(p[k], 1e-300) / niRef));
    }
    const double taun = ex[5], taup = ex[6], Et = ex[7], Cn = ex[8], Cp = ex[9], B = ex[10];
    const double excess = s.n * s.p - s.ni * s.ni * gn * gp;
    if (std::isfinite(taun) && std::isfinite(taup)) {
        const double kT = Vt * Q;
        const double n1 = s.ni * std::exp(Et / kT), p1 = s.ni * std::exp(-Et / kT);
        s.R += excess / (taup * (s.n + gn * n1) + taun * (s.p + gp * p1));
    }
    s.R += (Cn * s.n + Cp * s.p) * excess + B * excess;
    // The currents: each edge's Scharfetter-Gummel current, the vector that
    // best agrees with the three (least squares).
    const Mesh& mesh = sol.mesh();
    QPointF x[3];
    for (int k = 0; k < 3; ++k) x[k] = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * mesh.unitScale;
    double a11 = 0, a12 = 0, a22 = 0, bn[2] = {0, 0}, bp[2] = {0, 0};
    for (int k = 0; k < 3; ++k) {
        const int i = k, j = (k + 1) % 3;
        const double hx = x[j].x() - x[i].x(), hy = x[j].y() - x[i].y(), h = std::hypot(hx, hy);
        if (!(h > 0)) continue;
        const double ux = hx / h, uy = hy / h;
        const double dn = (V[j] + thn[j] - V[i] - thn[i]) / Vt, dp = (V[j] + thp[j] - V[i] - thp[i]) / Vt;
        const double jn = Q * s.mun * Vt / h * (n[j] * bern(dn) - n[i] * bern(-dn));
        const double jp = Q * s.mup * Vt / h * (p[i] * bern(dp) - p[j] * bern(-dp));
        a11 += ux * ux;
        a12 += ux * uy;
        a22 += uy * uy;
        bn[0] += ux * jn;
        bn[1] += uy * jn;
        bp[0] += ux * jp;
        bp[1] += uy * jp;
    }
    const double det = a11 * a22 - a12 * a12;
    if (std::abs(det) > 1e-30) {
        s.Jn[0] = (a22 * bn[0] - a12 * bn[1]) / det;
        s.Jn[1] = (a11 * bn[1] - a12 * bn[0]) / det;
        s.Jp[0] = (a22 * bp[0] - a12 * bp[1]) / det;
        s.Jp[1] = (a11 * bp[1] - a12 * bp[0]) / det;
    }
    return s;
}

} // namespace qucs_s::fem::detail
