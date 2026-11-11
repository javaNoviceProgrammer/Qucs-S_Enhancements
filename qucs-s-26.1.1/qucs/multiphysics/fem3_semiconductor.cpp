/*
 * fem3_semiconductor.cpp - semiconductors in 3D: the drift-diffusion
 *                          equations (Poisson's for the potential,
 *                          continuity for electrons and holes) by the box
 *                          method on a swept mesh's linear prisms - a plan
 *                          triangle's cotangent weights and corner shares
 *                          (the 2D solver's) times its layer, its vertical
 *                          edges the shares over its height -, each edge's
 *                          carrier current Scharfetter and Gummel's on the
 *                          carriers' effective potentials (a node's own
 *                          bands: heterojunctions graded, band gap
 *                          narrowing, the lattice's temperature, Fermi-
 *                          Dirac's degeneracy); solved together by Newton's
 *                          method from equilibrium, the contacts' voltages
 *                          raised in steps; in time (BDF), and small-signal
 *                          about a solution (the contacts' admittance).
 *                          Ohmic, Schottky and gate contacts, insulators;
 *                          mobilities constant, by doping (Caughey and
 *                          Thomas's, Masetti's, Arora's), by doping and
 *                          carriers (Dorkel and Leturcq's, Klaassen's), an
 *                          expression, saturated by the field along each
 *                          edge; recombination (SRH with doping-dependent
 *                          lifetimes and trap-assisted tunnelling, Auger,
 *                          direct), generation, impact ionization,
 *                          band-to-band tunnelling, incomplete ionization;
 *                          surface charge and recombination; the carriers'
 *                          heat for a heat physics
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_problem.h"

#include "fem3_mesh.h"
#include "fem3_wave.h"
#include "fem_materials.h"
#include "fem_problem.h"
#include "fem_semiconductor_models.h"

#include <QElapsedTimer>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <array>
#include <limits>
#include <map>

namespace qucs_s::fem::detail3 {

namespace {

/// Bernoulli's function x / (e^x - 1) and its derivative, safe at 0 and far
/// from it (the 2D solver's).
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
    const double e = std::expm1(x);
    return (e - x * (e + 1)) / (e * e);
}

constexpr double Q = 1.602176634e-19;    // C
constexpr double KB = 1.380649e-23;      // J/K
constexpr double Eps0 = 8.8541878188e-12;
constexpr double Mass0 = 9.1093837015e-31, HbarSI = 1.054571817e-34;
const double NaN = std::numeric_limits<double>::quiet_NaN();
const double Inf = std::numeric_limits<double>::infinity();

/// The field's components: the potential and the carriers, the doping, the
/// carriers' heat, the band edges and quasi-Fermi levels (J).
enum Component { CV = 0, Cn, Cp, CNd, CNa, CH, CEc, CEv, CEfn, CEfp, CR, CTn, CTp, Components };

/// A plan triangle's share of its box at corner P (m², \a s the mesh's
/// unit in metres): its circumcentric (Voronoi) area, or where it is obtuse
/// half to the obtuse corner and a quarter to each other - the 2D solver's
/// rule, so a model uniform in z is its 2D model's.
double cornerShare(const Vec3& P, const Vec3& Qv, const Vec3& R, double s)
{
    const double ux = (Qv.x - P.x) * s, uy = (Qv.y - P.y) * s, vx = (R.x - P.x) * s, vy = (R.y - P.y) * s;
    const double area = std::abs(ux * vy - uy * vx) / 2;
    if (!(area > 0)) return 0;
    const auto dot = [](double ax, double ay, double bx, double by) { return ax * bx + ay * by; };
    const double atP = dot(ux, uy, vx, vy);
    const double qpx = -ux, qpy = -uy, qrx = (R.x - Qv.x) * s, qry = (R.y - Qv.y) * s;
    const double atQ = dot(qpx, qpy, qrx, qry);
    const double rpx = -vx, rpy = -vy, rqx = -qrx, rqy = -qry;
    const double atR = dot(rpx, rpy, rqx, rqy);
    if (atP < 0) return area / 2;
    if (atQ < 0 || atR < 0) return area / 4;
    // (|PR|² cot Q + |PQ|² cot R)/8, cot = dot/(2 area).
    return (dot(vx, vy, vx, vy) * atQ + dot(ux, uy, ux, uy) * atR) / (16 * area);
}

/// A domain's medium: a semiconductor (its bands, mobilities and
/// recombination) or an insulator (its permittivity alone).
struct Medium {
    bool on = false;
    bool semiconductor = false;
    double epsr = 1;
    double Eg = 0, chi = 0, Nc = 0, Nv = 0, ni = 0, dEc = 0;   // J, 1/m³
    double mun = 0, mup = 0;                                   // m²/(V s): the lattice's
    enum class Mobility { Constant, Caughey, Masetti, Arora, Dorkel, Klaassen, Yamaguchi, Expression };
    Mobility mobility = Mobility::Constant;
    double ct[2][3] = {};                                      // Caughey and Thomas's µmin, Nref, α
    double ms[2][8] = {}, msMax[2] = {0, 0};                   // Masetti's
    double ar[2][8] = {}, dl[2][4] = {}, kl[2][5] = {};        // Arora's, Dorkel and Leturcq's, Klaassen's
    double klNrefD = 4e26, klNrefA = 7.2e26, klCD = 0.21, klCA = 0.5, klMe = 1, klMh = 1.258, klPmin[2] = {1, 1};
    Expression muExpr[2];
    double zetan = 0, zetap = 0;                               // (TL/T)^-ζ
    bool saturation = false;
    double vsatn = 1e5, vsatp = 8e4, betan = 2, betap = 1, vsatAn = 0, vsatAp = 0;
    bool vsatExp = false;
    bool fermi = false;
    bool bgn = false;
    double bgnE = 0, bgnN = 1, bgnC = 0.5;
    bool incomplete = false;
    double n1d = Inf, gd = 2, p1a = Inf, ga = 4;
    std::vector<double> lnNiAt;                                // ln ni at the lattice's temperatures (ThermalFrom...)
    bool srh = false;
    Expression taunOf, taupOf;
    double Et = 0;
    bool fossum = false;
    double N0n = 1, N0p = 1, fA = 1, fB = 0, fC = 1, fE = 1;
    bool tat = false;
    double FGn = Inf, FGp = Inf;
    double Cn = 0, Cp = 0, B = 0;                              // Auger's (m⁶/s), direct (m³/s)
    std::vector<Expression> generation;                        // 1/(m³ s)
    std::vector<double> generationPhoton;                      // each one's pairs' energy as made (J; NaN: the gap's)
    // Impact ionization's pairs in the equations (off: its integrals
    // alone), driven by the field, each carrier's quasi-Fermi gradient or
    // the field along its current.
    bool ionCurrents = true, ionFermi = false, ionAlong = false;
    bool tunnelling = false, btGeneration = false;
    double btA = 0, btB = 0, btGamma = 2.5;
    bool ionization = false;
    double an = 0, bn = 0, an2 = 0, bn2 = 0, ap = 0, bp = 0, ap2 = 0, bp2 = 0, E0 = Inf, ionBetaN = 1, ionBetaP = 1, ionPhonon = 0;
    double rHn = 1, rHp = 1;                                   // Hall factors: µH = r µ (a magnetic field's)
    // Yamaguchi's µL, S, Nref, as, uL, vs, G by carrier; its lcrit.
    double ya[2][7] = {};
    // Scattering at an insulator's surface, faded as e^(-d/lcrit) away from
    // it: Lombardi's B, C, λ, δ by carrier (C in cm, V, s and K), or
    // Shirahata's µ0, θ, E1, E2, P1, P2.
    bool surface = false, shirahata = false;
    double sl[2][4] = {}, sh[2][6] = {};
    double lcrit = 1e-8;
    // Saturation driven by the whole field (else the force along each
    // current), or by each carrier's temperature (an energy balance's,
    // relaxation times τw); the electrons' velocity the transferred
    // electrons' past E0.
    bool wholeField = false, energy = false;
    double tauWn = 0.3e-12, tauWp = 0.25e-12;
    bool transferred = false;
    double E0n = 4e5;
    bool exprState = false, exprAcross = false;               // an expression reads n, p, Epar, Eperp, dsurf; Eperp or dsurf
    bool varies() const
    {
        return saturation || surface || mobility == Mobility::Dorkel || mobility == Mobility::Klaassen || mobility == Mobility::Yamaguchi
               || (mobility == Mobility::Expression && exprState);
    }
    /// Whether it reads the field across the nearest interface.
    bool perpendicular() const { return surface || mobility == Mobility::Yamaguchi || (mobility == Mobility::Expression && exprAcross); }
};

/// A contact: its faces' nodes and their areas, and what it holds.
struct Contact {
    enum Kind { Ohmic, Schottky, Gate };
    Kind kind = Ohmic;
    QString name;
    Expression voltage;
    double phiB = 0, vn = 0, vp = 0;   // a Schottky contact's (J, m/s)
    double phim = 0;                   // a gate's work function (J)
    std::map<int, double> nodes;       // node: its share of the contact's area (m²)
    double applied = 0;                // V, as solved: a circuit's source's
    double current = 0;                // A, into the device (in time the metal's charge's change with it)
    double charge = 0;                 // C, on the contact
    // Its circuit: held at V0, or V0 through Rs and a load RL in series (at
    // rest and in time alike), or through a bias tee (Rs at rest, in time
    // RL carrying the current's change from rest); a capacitance Cp across
    // the device at the contact. Its own voltage then an unknown (iC),
    // solved with the carriers.
    enum Circuit { Held, Series, BiasTee };
    Circuit circuit = Held;
    double Rs = 0, RL = 0, Cp = 0;     // Ω, Ω, F
    int iC = -1;                       // its own voltage's unknown (-1: held)
    double own = 0;                    // its own voltage as solved (V)
    double teeCurrent = 0;             // a bias tee's current at rest (A)
    double capCurrent = 0;             // Cp's, Cp dVc/dt (A, in time)
    double restR() const { return circuit == Series ? Rs + RL : circuit == BiasTee ? Rs : 0.0; }
    /// Its load's current toward the device: the current through Rs (the
    /// device's and Cp's), or a bias tee's change from rest (none at rest).
    double loadCurrent(bool inTime) const
    {
        if (circuit == Series) return current + capCurrent;
        if (circuit == BiasTee) return inTime ? current + capCurrent - teeCurrent : 0.0;
        return 0.0;
    }
};

/// A face's surface charge, recombination or traps, lumped at its nodes.
/// Its recombination is a trap level of no charge (N 1, cn Sn, cp Sp).
struct Surface {
    std::map<int, double> nodes;   // node: its area (m²)
    double rhos = 0;               // C/m²
    struct Level {
        double Et = 0, N = 0;      // above the intrinsic level (J); 1/m²
    };
    std::vector<Level> levels;     // its traps' (none: a charge alone)
    double cn = 0, cp = 0;         // σ vth (m³/s), or Sn and Sp (m/s)
    bool charged = false;          // its traps' charge (false: recombination alone)
    bool acceptor = true;          // -q N f; else q N (1 - f)
    bool minorityOnly = false;     // at an ohmic contact its minority carriers alone
    QString name;
};

/// A trap level's occupancy f = (cn n + cp p1)/D, D = cn (n + n1) + cp (p +
/// p1), and the pairs it recombines a second, U = N cn cp (n p - ni²)/D, of
/// n, p - and their changes.
struct TrapState {
    double f = 0, D = 0, U = 0, dfdn = 0, dfdp = 0, dUdn = 0, dUdp = 0;
};
TrapState trapState(double N, double cn, double cp, double n1, double p1, double n, double p)
{
    TrapState t;
    t.D = cn * (n + n1) + cp * (p + p1);
    if (!(t.D > 0)) return t;
    t.f = (cn * n + cp * p1) / t.D;
    t.dfdn = cn * (1 - t.f) / t.D;
    t.dfdp = -cp * t.f / t.D;
    const double ex = n * p - n1 * p1;
    t.U = N * cn * cp * ex / t.D;
    t.dUdn = N * cn * cp * (p * t.D - ex * cn) / (t.D * t.D);
    t.dUdp = N * cn * cp * (n * t.D - ex * cp) / (t.D * t.D);
    return t;
}

/// Fowler-Nordheim tunnelling through an insulator: J = A E² e^(-B/E) at
/// the mean field over its barrier, along each face's normal into it.
struct FowlerNordheim {
    double A = 0, B = 0, phi = 0;   // A/V², V/m, φB/q (V)
    std::map<int, double> faces;    // the geometry's faces (by number, till the mesh's are known)
    struct Point {
        Vec3 x, n;                  // a mesh face's middle (m), its normal into the insulator
        double area = 0;            // m²
        int insulator = -1;         // its domain
    };
    std::vector<Point> points;
};

struct Edge {
    int i = -1, j = -1;
    double eps = 0;    // Σ ε0 εr × its coupling (F): Poisson's
    double wn = 0;     // Σ µn × its coupling (m³/(V s)): electrons', as the mobilities are now
    double wp = 0;     // holes'
    double length = 0; // m
    Vec3 t;            // its direction, i to j
    double geo = 0;    // Σ its semiconductor prisms' couplings (m): its dual face over its length
    double muN = 0, muP = 0;   // its prisms' largest mobilities (impact ionization's bound)
    // Its prisms' bands (an abrupt heterojunction's edge along the interface
    // two, one each side's): its carriers theirs at both ends - an end's
    // node on the other side's bands seen on these, its density times fN =
    // e^(anE - an), fP = e^(apE - ap) (the quasi-Fermi levels continuous).
    int medium = -1;
    double anI = 0, anJ = 0, apI = 0, apJ = 0;
    double fNi = 1, fNj = 1, fPi = 1, fPj = 1;
    // Into a heterojunction's step (thermionic, tunnelling): its end on the
    // interface, its face's share across it |t·n|, each carrier's
    // Richardson velocity, step (J) and tunnelling mass (kg) - its own
    // side's.
    bool emitsAtI = false;
    double across = 0, vRn = 0, vRp = 0, stepN = 0, stepP = 0, massN = 0, massP = 0;
    int domain = -1;   // a semiconductor domain beside it (its ionization's)
};

/// An edge's coupling through one prism (its mobilities' share).
struct EdgePart {
    int edge = -1, prism = -1;
    double c = 0;      // m
};

/// A semiconductor prism: its corners' doping's mean, its carriers' (as
/// last solved), its mobilities.
struct Prism {
    int domain = -1;
    double volume = 0;
    double Nd = 0, Na = 0;
    double mun = 0, mup = 0;   // low-field, at its lattice's temperature
    std::array<int, 9> edges{};        // its edges (a magnetic field's turned currents')
    std::array<double, 9> coupling{};  // its share of each one's dual face over its length (m)
    int edgeCount = 0;
    double grad[6][3] = {};            // its corners' shape functions' gradients at its middle (1/m)
    // The nearest interface with an insulator: its normal, its distance from
    // the prism's middle (m; infinite: none).
    double nrm[3] = {0, 0, 0};
    double dist = Inf;
};

class SemiconductorProblem3 : public Problem3
{
public:
    using Problem3::Problem3;
    bool setup(QString* error) override;
    bool solve(const Options3& o, QString* error) override;
    void setGlobals() override;
    bool hasMass() const override { return true; }
    bool solvedAtRestFirst() const override { return true; }
    /// The contacts' admittance at \a frequency (Hz) about the solution as it
    /// is: Y_kl = dI_k/dV_l + jω dQ_k/dV_l.
    bool smallSignal(double frequency, std::vector<std::vector<std::complex<double>>>* Y, QString* error);
    QStringList contactNames() const
    {
        QStringList out;
        for (const Contact& c : a_contacts) out << c.name;
        return out;
    }

private:
    bool geometry(QString* error);
    void nodeBands();
    /// The carriers at node \a i at u = V - φ (one Fermi level φ): n, p and
    /// their changes with u.
    void carriersAt(int i, double u, double* n, double* p, double* dn, double* dp) const;
    /// u of node \a i where its carriers are neutral with its doping.
    double neutralU(int i) const;
    /// Ionized donors and acceptors at node \a i of carriers n, p.
    void ionized(int i, double n, double p, double* nd, double* dnd, double* na, double* dna) const;
    /// The prisms' low-field mobilities and the edges' weights at \a x (its
    /// carriers and field; none: at rest).
    void mobilities(const Eigen::VectorXd* x);
    void assemble(const Eigen::VectorXd& x, Eigen::VectorXd& F, std::vector<Eigen::Triplet<double>>* J, bool carriers) const;
    bool newton(Eigen::VectorXd& x, const std::vector<double>& applied, bool carriers, int maxSteps, int* steps, QString* why);
    void heldValues(const std::vector<double>& applied, std::vector<double>& held) const;
    /// Contact \a c's current into the device at \a x, \a F its rows as
    /// assembled there (conduction; in time the metal's charge's change too)
    /// - and its change by unknown from \a trip, their entries, when asked.
    double contactCurrent(int c, const Eigen::VectorXd& x, const Eigen::VectorXd& F, const std::vector<Eigen::Triplet<double>>* trip,
                          std::map<int, double>* dI) const;
    /// The circuits' rows at \a x (V0 - Vc - R (I + Cp dVc/dt), a bias
    /// tee's RL I0 in time): into \a G, their Jacobian's entries into \a J.
    void circuitRows(const Eigen::VectorXd& x, const Eigen::VectorXd& F, const std::vector<Eigen::Triplet<double>>* trip, const std::vector<double>& applied,
                     bool carriers, Eigen::VectorXd& G, std::vector<Eigen::Triplet<double>>* J) const;
    /// The contacts' voltages their nodes are held at: a circuit's own (at
    /// \a x, with its carriers), else \a applied.
    std::vector<double> heldAt(const Eigen::VectorXd& x, const std::vector<double>& applied, bool carriers) const
    {
        std::vector<double> out = applied;
        for (std::size_t c = 0; c < a_contacts.size(); ++c)
            if (a_contacts[c].iC >= 0 && carriers && x.size() > a_contacts[c].iC) out[c] = x(a_contacts[c].iC);
        return out;
    }
    bool inTime() const { return a_mass != 0 && a_history; }
    /// A contact's stored value in the history (0 its own voltage, 1 its
    /// charge, 2 its tee's current at rest).
    double historyOf(int c, int k) const
    {
        const std::size_t at = std::size_t(Components * a_n + 3 * c + k);
        return a_history && a_history->size() > at ? (*a_history)[at] : 0.0;
    }
    void contactResults(const Eigen::VectorXd& x);
    double recombination(int i, double n, double p, double field, double* dn, double* dp) const;
    void store(const Eigen::VectorXd& x);
    bool rampTo(Eigen::VectorXd& x, const std::vector<double>& target, const Options3& o, QString* error);

    double a_T = 300, a_Vt = 0.025;
    // A magnetic flux density (T): each carrier's current turned, J = (J0 +
    // J0×β + (J0·β) β)/(1 + β²), β = ∓ r µ B (electrons', holes').
    double a_B[3] = {0, 0, 0};
    bool hall() const { return a_B[0] != 0 || a_B[1] != 0 || a_B[2] != 0; }
    /// Fermi-Dirac's degeneracy at each node at \a x: ln γ of its carriers
    /// (γ = F½(η)/e^η) and its change with them (scaled); Boltzmann's 0.
    void degeneracy(const Eigen::VectorXd& x, std::vector<double>& lgN, std::vector<double>& lgP, std::vector<double>* dlgN = nullptr,
                    std::vector<double>* dlgP = nullptr) const;
    /// Edge \a e's carriers at its ends on its own bands (scaled) and their
    /// steps Δ - electrons' of V + Vt an + Vt ln γ, holes' of V - Vt ap -
    /// Vt ln γ - over Vt.
    struct EdgeState {
        double ni = 0, nj = 0, pi = 0, pj = 0, Dn = 0, Dp = 0;
    };
    EdgeState edgeState(const Edge& e, const Eigen::VectorXd& x, const std::vector<double>& lgN, const std::vector<double>& lgP) const
    {
        EdgeState st;
        const int i = e.i, j = e.j;
        st.ni = x(3 * i + 1) * e.fNi;
        st.nj = x(3 * j + 1) * e.fNj;
        st.pi = x(3 * i + 2) * e.fPi;
        st.pj = x(3 * j + 2) * e.fPj;
        const double dv = (x(3 * j) - x(3 * i)) / a_Vt;
        st.Dn = dv + e.anJ - e.anI + lgN[std::size_t(j)] - lgN[std::size_t(i)];
        st.Dp = dv - e.apJ + e.apI - lgP[std::size_t(j)] + lgP[std::size_t(i)];
        return st;
    }
    /// The edges' bands at their ends and the interface nodes' boxes on the
    /// other sides' bands (a_boxN, a_boxP).
    void heteroSteps();
    /// Domain \a d's carriers' prefactors at node \a i (its bands there, its
    /// narrowing by the node's doping and lattice): an, ap and the narrowing.
    void bandsIn(int d, int i, double* an, double* ap, double* gap) const;
    std::vector<double> a_boxN, a_boxP;  // by node: its box, its other sides' parts on their bands
    std::vector<std::vector<std::pair<int, double>>> a_nodeParts;   // by node: (domain, its part of the box)
    std::vector<int> a_bandsOf;          // by domain: its bands' class (graded: one for all)
    int a_emission = 0;                  // heterojunctions: 0 drift-diffusion, 1 thermionic, 2 and tunnelling
    /// Impact ionization's pairs by prism, into its corners' rows.
    void impactIonization(const Eigen::VectorXd& x, const std::vector<double>& lgN, const std::vector<double>& lgP, Eigen::VectorXd& F,
                          const std::function<void(int, int, double)>& add) const;
    /// Each prism's currents turned by the field, into its edges' rows.
    void hallFluxes(const Eigen::VectorXd& x, const std::vector<double>& lgN, const std::vector<double>& lgP, const std::vector<double>& dlgN,
                    const std::vector<double>& dlgP, Eigen::VectorXd& F, const std::function<void(int, int, double)>& add,
                    std::vector<double>* edgeFlux = nullptr) const;
    /// By contact holding carriers (empty for a gate): a weight over the
    /// nodes, 1 on its own, 0 on the other such contacts', between them as
    /// the carriers' conductance at \a x has it - flat where they are many.
    /// A contact's current is the edges' currents times its weight's drop
    /// along them: its rows' reaction where the equations hold, but not the
    /// difference of a heavily doped contact's two large fluxes (2D's).
    std::vector<std::vector<double>> currentWeights(const Eigen::VectorXd& x) const;
    double a_C0 = 1;                     // the carriers' scale (1/m³)
    double a_refChi = 0, a_refDEc = 0;   // the reference semiconductor's (the first's)
    std::vector<Medium> a_media;         // by domain
    int a_n = 0;                         // nodes
    std::vector<char> a_nodeOn;          // in the physics
    std::vector<int> a_nodeMedium;       // its semiconductor's domain (-1: an insulator's alone, or none)
    std::vector<double> a_box;           // its semiconductor box (m³)
    std::vector<double> a_recBox;        // the parts of it on its own bands (m³): where it recombines (2D's)
    std::vector<double> a_Nd, a_Na;      // by node (1/m³)
    std::vector<double> a_generation;    // by node: G (1/(m³ s))
    std::vector<double> a_an, a_ap;      // by node: ln of its carriers' prefactors (u = V - φ: n = e^(u/Vt + an))
    std::vector<double> a_gap;           // by node: its narrowing (J)
    std::vector<double> a_tauN, a_tauP;  // by node (s)
    std::vector<double> a_TL;            // by node: the lattice's temperature (K)
    std::vector<double> a_field;         // by node: |E| (V/m), as last assembled from (trap-assisted tunnelling's)
    std::vector<Edge> a_edges;
    std::vector<EdgePart> a_parts;
    std::vector<Prism> a_prisms;         // by mesh prism (domain -1: no semiconductor)
    std::vector<Contact> a_contacts;
    std::vector<Surface> a_surfaces;
    std::vector<FowlerNordheim> a_fn;
    std::vector<int> a_heldKind;         // by node: -1 free, else its contact
    int a_unknowns = 0;                  // 3 a node, and each circuit's contact's own voltage
    std::vector<char> a_minorityFree;    // by node: an ohmic contact's whose minority carriers a surface's recombination takes
    std::vector<double> a_neutralU;
    double a_fermi = 0;                  // V: the Fermi level's potential at equilibrium (-Ef/q)
    Expression a_TLexpr;                 // the lattice's temperature (invalid: the physics' T)
    Scope a_exprScope;                   // a mobility expression's: N, Nd, Na, T, n, p, Epar, Eperp, dsurf
    int a_slotN = -1, a_slotNd = -1, a_slotNa = -1, a_slotT = -1, a_slotn = -1, a_slotp = -1, a_slotEpar = -1, a_slotEperp = -1, a_slotD = -1;
    // Hot carriers: each carrier's temperature by node (empty: none), its
    // last update's and its secant's, the updates' step.
    std::vector<double> a_Tn, a_Tp, a_TnWas, a_TpWas, a_dTnWas, a_dTpWas;
    double a_omegaT[2] = {1, 1}, a_residualT[2] = {0, 0};
    bool a_energyActive = true;          // (off on a walk: the field's mobilities, the temperatures at its end)
    bool hot() const
    {
        if (!a_energyActive) return false;
        for (const Medium& m : a_media)
            if (m.semiconductor && m.energy) return true;
        return false;
    }
    /// Each carrier's temperature by node from its energy balance at \a x.
    void carrierTemperatures(const Eigen::VectorXd& x);
    /// Prism \a e's field at its middle, -∇V (V/m), at \a x.
    void fieldIn(int e, const Eigen::VectorXd& x, double E[3]) const
    {
        const auto& pr = a_s.mesh().prisms[std::size_t(e)];
        E[0] = E[1] = E[2] = 0;
        for (int k = 0; k < 6; ++k)
            for (int c = 0; c < 3; ++c) E[c] -= a_prisms[std::size_t(e)].grad[k][c] * x(3 * pr[std::size_t(k)]);
    }
    // A step in time's (Options3's): what multiplies box ∂n/∂t, its history.
    double a_mass = 0;
    const std::vector<double>* a_history = nullptr;
    // Generation of t, by node: its expression and its share of the box (its
    // value again at each step in time).
    struct TimedGeneration {
        int node;
        Expression G;
        double share;
        double photon;   // its pairs' energy (J; NaN: the gap's)
    };
    // By node: the generation's pairs' energy, those of a photon's (W/m³)
    // and the rate of those of the gap's (1/(m³ s)), base and now.
    std::vector<double> a_genPhoton, a_genGapRate, a_genPhotonBase, a_genGapBase;
    // Stimulated recombination: on its domains c (⟨R⟩ - R0) above threshold,
    // ⟨R⟩ the domains' mean recombination at the last assembly.
    struct Stimulated {
        std::vector<char> on;   // by domain
        double c = 0, R0 = 0;
    };
    std::vector<Stimulated> a_stimulated;
    // Optical generation from a full-wave solve (a Wavelength Domain step's,
    // before this one): its light's |E| of 1 W fed, P0 of it, each photon
    // absorbed by the media's κ a pair at η.
    struct Optical {
        QString name, wave;
        Expression P0;
        double eta = 1;
        double RL = 50;         // a frequency step's load (Ω): HopRL
        std::vector<char> on;   // by domain
    };
    std::vector<Optical> a_optical;
    double a_Gop = 0, a_Pabs = 0;   // pairs a second, the light's power absorbed (W)
    std::vector<std::complex<double>> a_Hop;                    // by contact: its photocurrent, the light modulated wholly (A)
    std::vector<std::complex<double>> a_Hc;                     // the same through each circuit's load (A; empty: none)
    std::vector<double> a_noiseL;                                // by contact: the noise through its circuit's load (A²/Hz; NaN: none)
    std::vector<std::vector<std::complex<double>>> a_Y;          // the contacts' admittance at the last frequency
    double a_Pin = 0;                                            // the light fed (W)
    bool a_noise = false;                                        // noise in a frequency step (the physics' option)
    std::vector<std::vector<std::complex<double>>> a_noiseS;     // the contacts' short-circuit noise currents' correlations (A²/Hz)
    mutable std::vector<double> a_ionRate;                       // by prism: impact ionization's pairs (1/(m³ s)), as last assembled
    /// The optical generation at this time into a_generation (its pairs'
    /// energy, ħω/η, into a_genPhoton).
    bool opticalGeneration(QString* error);
    /// Behind circuits, at ω: the light's and the noise's currents through
    /// their loads (a_Hc, a_noiseL).
    void loads(double omega);
    mutable std::vector<double> a_lastR;     // by node: its recombination as last assembled (1/(m³ s))
    mutable std::vector<double> a_stimRate;  // by node: the stimulated rate as last assembled
    // As last assembled: impact ionization's pairs (1/s) and those made
    // otherwise - thermal, tunnelling, light, surfaces.
    mutable double a_ionPairs = 0, a_genPairs = 0;
    /// Impact ionization's integrals along the field's lines (after the
    /// solve): their globals.
    void ionizationIntegrals(const QString& p);
    std::vector<TimedGeneration> a_genTimed;
    std::vector<double> a_genBase;       // the generation not of t
    Eigen::VectorXd a_x;                 // the state as last solved
    bool a_solvedOnce = false;
};

int iV(int i) { return 3 * i; }
int iN(int i) { return 3 * i + 1; }
int iP(int i) { return 3 * i + 2; }

// ------------------------------------------------------------------ Setup

bool SemiconductorProblem3::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field3& f = field();
    const Model& model = a_s.model();
    const Topology3& topo = a_s.topology();
    const std::size_t D = topo.domains.size();
    const Scope& params = a_s.parameters().scope;
    QString why;
    const QVector<int> domains = resolveSelection3(topo, model, Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(D, 0);
    for (int d : domains)
        if (d >= 0 && d < int(D)) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    {
        const std::optional<double> T = evaluateConstant(a_node->text(QStringLiteral("T")).trimmed().isEmpty() ? QStringLiteral("300[K]") : a_node->text(QStringLiteral("T")),
                                                         params, Dim::temperature(), &why);
        if (!T || !(*T > 0)) problem(tr("%1: its temperature: %2").arg(a_node->name(), why));
        else a_T = *T;
    }
    a_Vt = KB * a_T / Q;
    a_noise = a_node->flag(QStringLiteral("noise"));
    // A magnetic field (Bz the plane's; Bx and By a 3D model's too).
    for (int c = 0; c < 3; ++c) {
        a_B[c] = 0;
        const QString key = QStringLiteral("B") + QLatin1Char("xyz"[c]);
        const QString b = a_node->text(key).trimmed();
        if (b.isEmpty()) continue;
        const std::optional<double> v = evaluateConstant(b, params, Dim::of(0, 1, -2, -1), &why);
        if (!v) problem(tr("%1: its magnetic flux density %2: %3").arg(a_node->name(), key, why));
        else a_B[c] = *v;
    }
    // The lattice's temperature, where it varies (a heat physics' ht.T).
    a_TLexpr = Expression();
    if (const QString tl = a_node->text(QStringLiteral("TL")); !tl.trimmed().isEmpty()) a_TLexpr = compile(tl, *a_node, QStringLiteral("TL"));
    // Heterojunctions: abrupt (each side's carriers on its own bands up to
    // the interface), its carriers emitted over the step (thermionic) or
    // tunnelling through its spike too, or graded over an element.
    const QString heterojunctions = a_node->text(QStringLiteral("heterojunctions"));
    const bool graded = heterojunctions == QLatin1String("element");
    a_emission = heterojunctions == QLatin1String("thermionic") ? 1 : heterojunctions == QLatin1String("tunnelling") ? 2 : 0;
    const std::vector<const Node*> material = materials();
    Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    coef.c.assign(D, {});
    coef.isotropic.assign(D, 1);
    coef.sources.assign(D, {});
    coef.sourceScale.assign(D, 1.0);
    coef.capacity.assign(D, Expression());
    coef.named.clear();
    a_media.assign(D, Medium());
    a_contacts.clear();
    a_surfaces.clear();
    a_fn.clear();
    a_stimulated.clear();
    a_optical.clear();
    a_Gop = a_Pabs = 0;
    a_lastR.clear();
    a_stimRate.clear();
    // (A mobility expression's variables.)
    a_exprScope = Scope(&params);
    a_slotN = a_exprScope.addVariable(QStringLiteral("N"), Dim::of(-3));
    a_slotNd = a_exprScope.addVariable(QStringLiteral("Nd"), Dim::of(-3));
    a_slotNa = a_exprScope.addVariable(QStringLiteral("Na"), Dim::of(-3));
    a_slotT = a_exprScope.addVariable(QStringLiteral("T"), Dim::temperature());
    a_slotn = a_exprScope.addVariable(QStringLiteral("n"), Dim::of(-3));
    a_slotp = a_exprScope.addVariable(QStringLiteral("p"), Dim::of(-3));
    a_slotEpar = a_exprScope.addVariable(QStringLiteral("Epar"), Dim::voltage() / Dim::length());
    a_slotEperp = a_exprScope.addVariable(QStringLiteral("Eperp"), Dim::voltage() / Dim::length());
    a_slotD = a_exprScope.addVariable(QStringLiteral("dsurf"), Dim::length());
    std::vector<const Node*> modelOf(D, nullptr), srhOf(D, nullptr), augerOf(D, nullptr), directOf(D, nullptr), btbtOf(D, nullptr), impactOf(D, nullptr);
    struct Doping {
        bool donor = false;
        QString profile;
        Expression N;            // uniform, expression
        double N0 = 0, sigma = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0, z0 = 0, z1 = 0;   // box
        double dose = 0, Rp = 0, sd = 0, sl = 0, zs = 0, w1 = -Inf, w2 = Inf;      // implant
        std::vector<char> on;
    };
    std::vector<Doping> dopings;
    const auto number = [&](const QString& text, const Dim& dim, const QString& where) {
        QString w;
        const std::optional<double> v = evaluateConstant(text, params, dim, &w);
        if (!v) {
            problem(tr("%1: %2").arg(where, w));
            return NaN;
        }
        return *v;
    };
    const auto energy = [&](const QString& text, const QString& where) {
        const Expression e = Expression::compile(text, params);
        if (!e.isValid() || !e.isConstant()) {
            problem(tr("%1: %2").arg(where, e.isValid() ? tr("%1 must be a constant").arg(text) : e.error()));
            return NaN;
        }
        return e.dim() == Dim::of(2, 1, -2) ? e.constant() : e.constant() * Q;
    };
    const double unit = a_s.mesh().unitScale;
    for (const Node& feature : a_node->children) {
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k) continue;
        const QVector<int> picked = resolveSelection3(topo, model, k->selection, feature.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(feature.name(), why));
        const QString t = feature.type;
        const auto onDomains = [&](std::vector<const Node*>& of) {
            for (int d : picked)
                if (d >= 0 && d < int(D)) of[std::size_t(d)] = &feature;
        };
        if (t == QLatin1String("semiconductor/model")) onDomains(modelOf);
        else if (t == QLatin1String("semiconductor/srh")) onDomains(srhOf);
        else if (t == QLatin1String("semiconductor/auger")) onDomains(augerOf);
        else if (t == QLatin1String("semiconductor/radiative")) onDomains(directOf);
        else if (t == QLatin1String("semiconductor/btbt")) onDomains(btbtOf);
        else if (t == QLatin1String("semiconductor/impact")) onDomains(impactOf);
        else if (t == QLatin1String("semiconductor/doping")) {
            Doping dp;
            dp.donor = feature.text(QStringLiteral("dopant")) == QLatin1String("donor");
            dp.profile = feature.text(QStringLiteral("profile"));
            if (dp.profile.isEmpty()) dp.profile = QStringLiteral("uniform");
            const auto len = [&](const QString& key) { return number(feature.text(key), Dim::length(), feature.name()); };
            if (dp.profile == QLatin1String("uniform")) dp.N = compile(feature.text(QStringLiteral("N0")), feature, QStringLiteral("N0"), false);
            else if (dp.profile == QLatin1String("expression")) dp.N = compile(feature.text(QStringLiteral("N")), feature, QStringLiteral("N"));
            else if (dp.profile == QLatin1String("box")) {
                // A box - its plan rectangle and (3D) its z range -, its
                // Gaussian fall outside it reaching the background at the
                // junction depth.
                dp.N0 = number(feature.text(QStringLiteral("N0")), Dim::of(-3), feature.name());
                const QJsonArray base = feature.value(QStringLiteral("position")).toArray(), size = feature.value(QStringLiteral("size")).toArray();
                const auto lengthOf = [&](const QJsonValue& v) {
                    const std::optional<double> c = evaluateConstant(v.toVariant().toString(), params, Dim(), &why);
                    return c ? *c * unit : 0.0;
                };
                dp.x0 = lengthOf(base.at(0));
                dp.y0 = lengthOf(base.at(1));
                dp.x1 = dp.x0 + lengthOf(size.at(0));
                dp.y1 = dp.y0 + lengthOf(size.at(1));
                if (dp.x1 < dp.x0) std::swap(dp.x0, dp.x1);
                if (dp.y1 < dp.y0) std::swap(dp.y0, dp.y1);
                const QString zb = feature.text(QStringLiteral("zmin")), zt = feature.text(QStringLiteral("zmax"));
                dp.z0 = zb.trimmed().isEmpty() ? -Inf : number(zb, Dim::length(), feature.name());
                dp.z1 = zt.trimmed().isEmpty() ? Inf : number(zt, Dim::length(), feature.name());
                const double dj = len(QStringLiteral("dj"));
                const double Nb = number(feature.text(QStringLiteral("Nb")), Dim::of(-3), feature.name());
                if (!(dj > 0) || !(Nb > 0) || !(dp.N0 > Nb))
                    problem(tr("%1: its junction depth, background and peak must be more than 0, the peak more than the background").arg(feature.name()));
                else dp.sigma = dj / std::sqrt(std::log(dp.N0 / Nb));
            } else if (dp.profile == QLatin1String("implant")) {
                // An implant: a Gaussian of its dose about its range below
                // its surface (in 3D at z, the ions downward), its image,
                // its mask's window in x spread by its lateral straggle,
                // annealed (σ² + 2 D t).
                dp.dose = number(feature.text(QStringLiteral("dose")), Dim::of(-2), feature.name());
                dp.Rp = len(QStringLiteral("Rp"));
                const double dRp = len(QStringLiteral("dRp")), dRl = len(QStringLiteral("dRl"));
                dp.zs = number(feature.text(QStringLiteral("surface")).trimmed().isEmpty() ? QStringLiteral("0") : feature.text(QStringLiteral("surface")), Dim(), feature.name()) * unit;
                const QString x1 = feature.text(QStringLiteral("x1")), x2 = feature.text(QStringLiteral("x2"));
                if (!x1.trimmed().isEmpty()) dp.w1 = number(x1, Dim(), feature.name()) * unit;
                if (!x2.trimmed().isEmpty()) dp.w2 = number(x2, Dim(), feature.name()) * unit;
                const double D0 = number(feature.text(QStringLiteral("D0")), Dim::of(2, 0, -1), feature.name());
                const double Ea = energy(feature.text(QStringLiteral("Ea")), feature.name());
                const double Ta = number(feature.text(QStringLiteral("Ta")), Dim::temperature(), feature.name());
                const double ta = number(feature.text(QStringLiteral("ta")), Dim::of(0, 0, 1), feature.name());
                const double Dt = ta > 0 ? D0 * std::exp(-Ea / (KB * Ta)) * ta : 0.0;
                dp.sd = std::sqrt(dRp * dRp + 2 * Dt);
                dp.sl = std::sqrt(dRl * dRl + 2 * Dt);
                if (!(dp.sd > 0) || !(dp.dose >= 0)) problem(tr("%1: its straggle must be more than 0").arg(feature.name()));
            } else {
                problem(tr("%1: a doping profile of %2 is not taken in 3D yet - uniform, a box, an implant or an expression is").arg(feature.name(), dp.profile));
                continue;
            }
            dp.on.assign(D, 0);
            for (int d : picked)
                if (d >= 0 && d < int(D)) dp.on[std::size_t(d)] = 1;
            dopings.push_back(dp);
        } else if (t == QLatin1String("semiconductor/generation")) {
            const Expression g = compile(feature.text(QStringLiteral("G")), feature, QStringLiteral("G"));
            // (Its pairs' energy as made: its photons', given, else the
            // gap's - their heat, semi.H's.)
            double photon = NaN;
            if (const QString eph = feature.text(QStringLiteral("Eph")).trimmed(); !eph.isEmpty()) {
                photon = energy(eph, feature.name());
                if (!(photon > 0)) problem(tr("%1: its photon energy must be more than 0").arg(feature.name()));
            }
            if (g.isValid())
                for (int d : picked)
                    if (d >= 0 && d < int(D)) {
                        a_media[std::size_t(d)].generation.push_back(g);
                        a_media[std::size_t(d)].generationPhoton.push_back(photon);
                    }
        } else if (t == QLatin1String("semiconductor/optical")) {
            Optical o;
            o.name = feature.name();
            o.wave = feature.text(QStringLiteral("mode")).trimmed();
            if (o.wave.isEmpty()) o.wave = QStringLiteral("ewfd");
            o.P0 = compile(feature.text(QStringLiteral("P0")).trimmed().isEmpty() ? QStringLiteral("1[mW]") : feature.text(QStringLiteral("P0")), feature, QStringLiteral("P0"), false);
            o.eta = feature.text(QStringLiteral("eta")).trimmed().isEmpty() ? 1.0 : number(feature.text(QStringLiteral("eta")), Dim(), feature.name());
            o.RL = feature.text(QStringLiteral("RL")).trimmed().isEmpty() ? 50.0 : number(feature.text(QStringLiteral("RL")), Dim::resistance(), feature.name());
            if (!(o.eta > 0) || o.eta > 1) problem(tr("%1: its internal quantum efficiency from more than 0 to 1").arg(feature.name()));
            o.on.assign(D, 0);
            for (int d : picked)
                if (d >= 0 && d < int(D)) o.on[std::size_t(d)] = 1;
            a_optical.push_back(o);
        } else if (t == QLatin1String("semiconductor/stimulated")) {
            // Stimulated recombination: c (⟨R⟩ - Jth/(q t)) above threshold
            // on its domains (as set, else its material's cstim, jthreshold,
            // tgain).
            Stimulated st;
            st.on.assign(D, 0);
            for (int d : picked)
                if (d >= 0 && d < int(D)) st.on[std::size_t(d)] = 1;
            const std::vector<const Node*> mats = materials();
            const Node* mat = picked.isEmpty() || picked.front() >= int(mats.size()) ? nullptr : mats[std::size_t(picked.front())];
            const auto param = [&](const QString& key, const QString& materialKey, const QString& fallback) {
                const QString t2 = feature.text(key).trimmed();
                if (!t2.isEmpty()) return t2;
                const QString mt = mat ? detail::Problem::materialText(*mat, materialKey, &params, a_T).trimmed() : QString();
                return mt.isEmpty() ? fallback : mt;
            };
            st.c = number(param(QStringLiteral("c"), QStringLiteral("cstim"), QStringLiteral("0.5")), Dim(), feature.name());
            const double Jth = number(param(QStringLiteral("Jth"), QStringLiteral("jthreshold"), QStringLiteral("1e7[A/m^2]")), Dim::of(-2, 0, 0, 1), feature.name());
            const double thickness = number(param(QStringLiteral("t"), QStringLiteral("tgain"), QStringLiteral("10[nm]")), Dim::length(), feature.name());
            if (!(st.c >= 0 && st.c <= 1) || !(Jth >= 0) || !(thickness > 0))
                problem(tr("%1: its share c from 0 to 1, its threshold no less than 0, its thickness more than 0").arg(feature.name()));
            else st.R0 = Jth / (Q * thickness);
            a_stimulated.push_back(st);
        } else if (t == QLatin1String("semiconductor/ohmic") || t == QLatin1String("semiconductor/schottky") || t == QLatin1String("semiconductor/gate")) {
            Contact c;
            c.kind = t.endsWith(QLatin1String("ohmic")) ? Contact::Ohmic : t.endsWith(QLatin1String("schottky")) ? Contact::Schottky : Contact::Gate;
            c.name = feature.text(QStringLiteral("name"));
            if (c.name.trimmed().isEmpty()) c.name = feature.name();
            c.voltage = compile(feature.text(QStringLiteral("V0")).trimmed().isEmpty() ? QStringLiteral("0[V]") : feature.text(QStringLiteral("V0")), feature,
                                QStringLiteral("V0"), false);
            if (c.kind == Contact::Schottky) {
                c.phiB = energy(feature.text(QStringLiteral("phiB")), feature.name());
                c.vn = number(feature.text(QStringLiteral("vn")), Dim::of(1, 0, -1), feature.name());
                c.vp = number(feature.text(QStringLiteral("vp")), Dim::of(1, 0, -1), feature.name());
            }
            if (c.kind == Contact::Gate) c.phim = energy(feature.text(QStringLiteral("phim")), feature.name());
            if (c.kind != Contact::Gate) {
                const QString circuit = feature.text(QStringLiteral("circuit"));
                c.circuit = circuit == QLatin1String("series") ? Contact::Series : circuit == QLatin1String("biastee") ? Contact::BiasTee : Contact::Held;
                if (c.circuit != Contact::Held) {
                    const auto ohms = [&](const QString& key, const Dim& dim) {
                        const QString t = feature.text(key);
                        return t.trimmed().isEmpty() ? 0.0 : number(t, dim, feature.name());
                    };
                    c.Rs = ohms(QStringLiteral("Rs"), Dim::resistance());
                    c.RL = ohms(QStringLiteral("RL"), Dim::resistance());
                    c.Cp = ohms(QStringLiteral("Cp"), Dim::capacitance());
                    if (!(c.Rs >= 0) || !(c.RL >= 0) || !(c.Cp >= 0)) problem(tr("%1: its circuit's Rs, RL and Cp must be 0 or more").arg(feature.name()));
                }
            }
            for (int b : picked) c.nodes[-1 - b] = 0;   // (its faces, by number, till the mesh's nodes are known)
            a_contacts.push_back(c);
        } else if (t == QLatin1String("semiconductor/charge") || t == QLatin1String("semiconductor/surfacerec") || t == QLatin1String("semiconductor/traps")) {
            Surface s;
            s.name = feature.name();
            if (t.endsWith(QLatin1String("charge"))) {
                const Expression r = compile(feature.text(QStringLiteral("rhos")), feature, QStringLiteral("rhos"), false);
                s.rhos = r.isValid() && r.isConstant() ? r.constant() : 0;
            } else if (t.endsWith(QLatin1String("surfacerec"))) {
                // A trap level of no charge: N 1, cn Sn, cp Sp - (n p - ni²)/((n
                // + n1)/Sp + (p + p1)/Sn) per area; of both carriers, or (its
                // "Apply to majority carriers" off) of an ohmic contact's
                // minority carriers alone.
                s.cn = number(feature.text(QStringLiteral("Sn")), Dim::of(1, 0, -1), feature.name());
                s.cp = number(feature.text(QStringLiteral("Sp")), Dim::of(1, 0, -1), feature.name());
                const double Et = feature.text(QStringLiteral("Et")).trimmed().isEmpty() ? 0.0 : energy(feature.text(QStringLiteral("Et")), feature.name());
                s.minorityOnly = feature.props.contains(QStringLiteral("majority")) && !feature.flag(QStringLiteral("majority"));
                if (!(s.cn >= 0) || !(s.cp >= 0) || !std::isfinite(Et)) problem(tr("%1: its recombination velocities no less than 0, its level given").arg(feature.name()));
                else if (s.cn > 0 && s.cp > 0) s.levels.push_back({Et, 1.0});
            } else {
                // Interface traps: a level, or a range in equal steps (each
                // Dit ΔE at its middle); each filled as Shockley, Read and
                // Hall have it.
                const double sn = number(feature.text(QStringLiteral("sigman")), Dim::area(), feature.name());
                const double sp = number(feature.text(QStringLiteral("sigmap")), Dim::area(), feature.name());
                const double vth = number(feature.text(QStringLiteral("vth")), Dim::of(1, 0, -1), feature.name());
                if (!(sn > 0) || !(sp > 0) || !(vth > 0)) {
                    problem(tr("%1: its cross sections and thermal velocity must be more than 0").arg(feature.name()));
                    continue;
                }
                s.cn = sn * vth;
                s.cp = sp * vth;
                s.charged = true;
                s.acceptor = feature.text(QStringLiteral("kind")) != QLatin1String("donor");
                if (feature.text(QStringLiteral("distribution")) == QLatin1String("level")) {
                    const double Nit = number(feature.text(QStringLiteral("Nit")), Dim::of(-2), feature.name());
                    const double Et = energy(feature.text(QStringLiteral("Et")).trimmed().isEmpty() ? QStringLiteral("0[eV]") : feature.text(QStringLiteral("Et")), feature.name());
                    if (!(Nit >= 0) || !std::isfinite(Et)) problem(tr("%1: its density and level").arg(feature.name()));
                    else s.levels.push_back({Et, Nit});
                } else {
                    const double Dit = number(feature.text(QStringLiteral("Dit")), Dim::of(-4, -1, 2), feature.name());
                    const double lo = energy(feature.text(QStringLiteral("Elow")), feature.name());
                    const double hi = energy(feature.text(QStringLiteral("Ehigh")), feature.name());
                    const int count = feature.integer(QStringLiteral("levels"));
                    if (!(Dit >= 0) || !std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo)) problem(tr("%1: its density and range").arg(feature.name()));
                    else if (count < 1 || count > 1000) problem(tr("%1: its levels from 1 to 1000, not %2").arg(feature.name()).arg(count));
                    else
                        for (int k = 0; k < count; ++k) s.levels.push_back({lo + (k + 0.5) * (hi - lo) / count, Dit * (hi - lo) / count});
                }
            }
            for (int b : picked) s.nodes[-1 - b] = 0;
            a_surfaces.push_back(s);
        } else if (t == QLatin1String("semiconductor/fn")) {
            // Lenzlinger and Snow's: A = q³ m0/(8π h φB m_ox), B = 8π √(2 m_ox)
            // φB^(3/2)/(3 q h).
            constexpr double Hp = 6.62607015e-34;
            const double phiB = energy(feature.text(QStringLiteral("phiB")), feature.name());
            const double mox = number(feature.text(QStringLiteral("mox")), Dim(), feature.name());
            if (!(phiB > 0) || !(mox > 0)) {
                problem(tr("%1: its barrier and mass must be more than 0").arg(feature.name()));
                continue;
            }
            FowlerNordheim fn;
            fn.A = Q * Q * Q / (8 * M_PI * Hp * phiB * mox);
            fn.B = 8 * M_PI * std::sqrt(2 * mox * Mass0) * std::pow(phiB, 1.5) / (3 * Q * Hp);
            fn.phi = phiB / Q;
            for (int b : picked) fn.faces[b] = 0;
            a_fn.push_back(fn);
        } else if (t == QLatin1String("semiconductor/polarization")) {
            // Wurtzite's polarization at its interfaces (c along the axis
            // chosen; the metal face's or the nitrogen's): each one's bound
            // charge (P_back - P_front)·n, n from its back into its front -
            // its sides' P = Psp + e31 (εxx + εyy) + e33 εzz, their nitride
            // compounds' pseudomorphic on the substrate's lattice in the
            // c-plane (none: relaxed). Its faces planar.
            const QString axis = feature.text(QStringLiteral("axis"));
            const int along = axis == QLatin1String("x") ? 0 : axis == QLatin1String("z") ? 2 : 1;
            const double face = feature.text(QStringLiteral("polarity")) == QLatin1String("nitrogen") ? -1.0 : 1.0;
            double lattice = NaN;
            const QString sub = feature.text(QStringLiteral("substrate")).trimmed();
            if (!sub.isEmpty()) {
                QString compound = sub;
                double sx = 0, sy = 0;
                for (const Node& mm : model.materials().children)
                    if (mm.name().compare(sub, Qt::CaseInsensitive) == 0 || mm.text(QStringLiteral("library")).compare(sub, Qt::CaseInsensitive) == 0) {
                        compound = detail::Problem::materialProperty(mm, QStringLiteral("compound"));
                        sx = evaluateConstant(detail::Problem::materialProperty(mm, QStringLiteral("x")), params, Dim()).value_or(0);
                        sy = evaluateConstant(detail::Problem::materialProperty(mm, QStringLiteral("y")), params, Dim()).value_or(0);
                    }
                if (const std::optional<MaterialEntry> e = findMaterial(sub); e && compound == sub) {
                    compound = e->properties.value(QStringLiteral("compound")).toString();
                    sx = evaluateConstant(e->properties.value(QStringLiteral("x")).toString(), params, Dim()).value_or(0);
                    sy = evaluateConstant(e->properties.value(QStringLiteral("y")).toString(), params, Dim()).value_or(0);
                }
                if (const std::optional<WurtziteParameters> w = wurtziteParameters(compound, sx, a_T, nullptr, sy)) lattice = w->a;
                else lattice = number(sub, Dim::length(), tr("%1's substrate (a nitride or a lattice constant)").arg(feature.name()));
            }
            const std::vector<const Node*> mats = materials();
            const auto polarizationOf = [&](int d) -> std::optional<double> {
                const Node* mat = d >= 0 && d < int(mats.size()) ? mats[std::size_t(d)] : nullptr;
                if (!mat) return std::nullopt;
                const QString compound = detail::Problem::materialProperty(*mat, QStringLiteral("compound")).trimmed();
                const double x = evaluateConstant(detail::Problem::materialProperty(*mat, QStringLiteral("x")), params, Dim()).value_or(0);
                const double y = evaluateConstant(detail::Problem::materialProperty(*mat, QStringLiteral("y")), params, Dim()).value_or(0);
                const std::optional<WurtziteParameters> w = isWurtzite(compound) ? wurtziteParameters(compound, x, a_T, nullptr, y) : std::nullopt;
                if (!w) return std::nullopt;
                const double exx = std::isfinite(lattice) ? (lattice - w->a) / w->a : 0, ezz = -2 * w->C13 / w->C33 * exx;
                return w->Psp + 2 * w->e31 * exx + w->e33 * ezz;
            };
            for (int b : picked) {
                if (b < 0 || b >= int(topo.faces.size())) continue;
                const Topology3::Face& fc = topo.faces[std::size_t(b)];
                const std::optional<double> pb = polarizationOf(fc.back), pf = polarizationOf(fc.front);
                if (!pb || !pf) {
                    problem(tr("%1: face %2 is not between two nitrides (their materials' compounds GaN, AlGaN, InGaN...)").arg(feature.name()).arg(b + 1));
                    continue;
                }
                if (!fc.planar) {
                    problem(tr("%1: face %2 is curved: a polarization charge's interface is planar").arg(feature.name()).arg(b + 1));
                    continue;
                }
                const double nc = along == 0 ? fc.normal.x : along == 1 ? fc.normal.y : fc.normal.z;
                Surface sf;
                sf.name = feature.name();
                sf.rhos = face * (*pb - *pf) * nc;
                sf.nodes[-1 - b] = 0;
                a_surfaces.push_back(sf);
                a_s.log << tr("%1: face %2's polarization charge %3").arg(feature.name()).arg(b + 1).arg(formatQuantity(sf.rhos, Dim::of(-2, 0, 1, 1)));
            }
        } else if (t != QLatin1String("semiconductor/insulation")) {
            problem(tr("%1: %2 is not solved in 3D yet").arg(feature.name(), k->title));
        }
    }
    // Each domain's medium: a semiconductor where its band gap is given, an
    // insulator (its permittivity alone) where its material has none.
    bool referenced = false;
    const Dim conc = Dim::of(-3), mobility = Dim::of(0, -1, 2, 1), velocity = Dim::of(1, 0, -1);
    for (int d = 0; d < int(D); ++d) {
        if (!f.domains[std::size_t(d)]) continue;
        Medium& m = a_media[std::size_t(d)];
        m.on = true;
        const Node* mf = modelOf[std::size_t(d)];
        const Node* mat = material[std::size_t(d)];
        const QString where = tr("domain %1 (%2)").arg(d + 1).arg(topo.objectNames.value(topo.domains[std::size_t(d)].owner));
        const bool user = mf && mf->text(QStringLiteral("source")) == QLatin1String("user");
        if (!user && !mat) {
            problem(tr("%1 has no material").arg(where));
            continue;
        }
        auto text = [&](const QString& key) { return user ? mf->text(key) : detail::Problem::materialText(*mat, key, &params, a_T); };
        const QString eps = text(QStringLiteral("epsilonr"));
        if (eps.trimmed().isEmpty()) problem(tr("%1: its material has no relative permittivity").arg(where));
        else m.epsr = number(eps, Dim(), where);
        m.semiconductor = !text(QStringLiteral("Eg")).trimmed().isEmpty();
        if (!m.semiconductor) continue;
        m.Eg = energy(text(QStringLiteral("Eg")), where);
        const QString chi = text(QStringLiteral("chi"));
        m.chi = chi.trimmed().isEmpty() ? 4.05 * Q : energy(chi, where);
        for (const auto& [key, member, dim] : {std::tuple{QStringLiteral("Nc"), &Medium::Nc, conc}, std::tuple{QStringLiteral("Nv"), &Medium::Nv, conc},
                                               std::tuple{QStringLiteral("mun"), &Medium::mun, mobility}, std::tuple{QStringLiteral("mup"), &Medium::mup, mobility}}) {
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
            a_refChi = m.chi;
            a_refDEc = m.dEc;
            referenced = true;
        }
        // (Its intrinsic density at the lattice's temperatures, where they
        // vary: its own texts at each.)
        if (a_TLexpr.isValid()) {
            for (int k = 0; k < detail::ThermalPoints; ++k) {
                const double Tk = detail::ThermalFrom + detail::ThermalStep * k;
                auto at = [&](const QString& key) { return user ? mf->text(key) : detail::Problem::materialText(*mat, key, &params, Tk); };
                QString w;
                const double eg = [&] {
                    const Expression e = Expression::compile(at(QStringLiteral("Eg")), params);
                    return e.isValid() && e.isConstant() ? (e.dim() == Dim::of(2, 1, -2) ? e.constant() : e.constant() * Q) : NaN;
                }();
                const std::optional<double> nc = evaluateConstant(at(QStringLiteral("Nc")), params, conc, &w);
                const std::optional<double> nv = evaluateConstant(at(QStringLiteral("Nv")), params, conc, &w);
                if (!(eg > 0) || !nc || !nv || !(*nc > 0) || !(*nv > 0)) {
                    problem(tr("%1: its band gap and densities of states at %2 K, for its lattice temperature: %3").arg(where).arg(Tk).arg(w));
                    m.lnNiAt.clear();
                    break;
                }
                m.lnNiAt.push_back(0.5 * std::log(*nc * *nv) - eg / (2 * KB * Tk));
            }
        }
        const auto v = [&](const QString& key, const Dim& dim, const QString& model) {
            return number(mf ? mf->text(key) : QString(), dim, tr("%1: %2, %3").arg(where, model, key));
        };
        const QString mob = mf ? mf->text(QStringLiteral("mobility")) : QString();
        using M = Medium::Mobility;
        if (mob == QLatin1String("doping")) {
            m.mobility = M::Caughey;
            const char* keys[2][3] = {{"munmin", "nrefn", "alphan"}, {"mupmin", "nrefp", "alphap"}};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 3; ++k) m.ct[c][k] = v(QString::fromLatin1(keys[c][k]), k == 0 ? mobility : k == 1 ? conc : Dim(), tr("Caughey and Thomas's mobility"));
        } else if (mob == QLatin1String("masetti")) {
            m.mobility = M::Masetti;
            const char* keys[2][8] = {{"m1n", "m2n", "mu1n", "pcn", "crn", "csn", "man", "mbn"}, {"m1p", "m2p", "mu1p", "pcp", "crp", "csp", "map", "mbp"}};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 8; ++k) m.ms[c][k] = v(QString::fromLatin1(keys[c][k]), k < 3 ? mobility : k < 6 ? conc : Dim(), tr("Masetti's mobility"));
            for (int c = 0; c < 2; ++c) {
                const QString t = mf->text(c == 0 ? QStringLiteral("mmaxn") : QStringLiteral("mmaxp"));
                m.msMax[c] = t.trimmed().isEmpty() ? (c == 0 ? m.mun : m.mup) : number(t, mobility, where);
            }
        } else if (mob == QLatin1String("arora")) {
            m.mobility = M::Arora;
            const char* keys[2][8] = {{"arminn", "aran", "ardn", "arbn", "arnrefn", "arcn", "aralphan", "ardexpn"},
                                      {"arminp", "arap", "ardp", "arbp", "arnrefp", "arcp", "aralphap", "ardexpp"}};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 8; ++k) m.ar[c][k] = v(QString::fromLatin1(keys[c][k]), k == 0 || k == 2 ? mobility : k == 4 ? conc : Dim(), tr("Arora's mobility"));
        } else if (mob == QLatin1String("dorkel")) {
            m.mobility = M::Dorkel;
            const char* keys[2][4] = {{"dlmun", "dlgamman", "dlan", "dlbn"}, {"dlmup", "dlgammap", "dlap", "dlbp"}};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 4; ++k) m.dl[c][k] = v(QString::fromLatin1(keys[c][k]), k == 0 ? mobility : Dim(), tr("Dorkel and Leturcq's mobility"));
        } else if (mob == QLatin1String("klaassen")) {
            m.mobility = M::Klaassen;
            const QString kl = tr("Klaassen's mobility");
            const char* keys[2][5] = {{"klmaxn", "klminn", "klthetan", "klnrefn", "klalphan"}, {"klmaxp", "klminp", "klthetap", "klnrefp", "klalphap"}};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 5; ++k) m.kl[c][k] = v(QString::fromLatin1(keys[c][k]), k < 2 ? mobility : k == 3 ? conc : Dim(), kl);
            m.klNrefD = v(QStringLiteral("klnrefd"), conc, kl);
            m.klNrefA = v(QStringLiteral("klnrefa"), conc, kl);
            m.klCD = v(QStringLiteral("klcd"), Dim(), kl);
            m.klCA = v(QStringLiteral("klca"), Dim(), kl);
            m.klMe = v(QStringLiteral("klme"), Dim(), kl);
            m.klMh = v(QStringLiteral("klmh"), Dim(), kl);
            m.klPmin[0] = detail::klaassenPmin(a_T, m.klMe);
            m.klPmin[1] = detail::klaassenPmin(a_T, m.klMh);
        } else if (mob == QLatin1String("yamaguchi")) {
            m.mobility = M::Yamaguchi;
            const QString yamaguchi = tr("Yamaguchi's mobility");
            const Dim perField = Dim::of(-1, -1, 3, 1);
            const char* keys[2][7] = {{"ymuln", "ysn", "ynrefn", "yasn", "yuln", "yvsn", "ygn"}, {"ymulp", "ysp", "ynrefp", "yasp", "yulp", "yvsp", "ygp"}};
            const Dim dims[7] = {mobility, Dim(), conc, perField, velocity, velocity, Dim()};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 7; ++k) m.ya[c][k] = v(QString::fromLatin1(keys[c][k]), dims[k], yamaguchi);
            m.lcrit = v(QStringLiteral("ylcrit"), Dim::length(), yamaguchi);
            for (int c = 0; c < 2; ++c) {
                bool positive = true;
                for (int k = 0; k < 7; ++k) positive = positive && (k == 3 ? m.ya[c][k] >= 0 : m.ya[c][k] > 0);
                if (!positive) problem(tr("%1: Yamaguchi's µL, S, Nref, uL, vs and G must be more than 0, as at least 0").arg(where));
            }
            if (!(m.lcrit > 0)) problem(tr("%1: Yamaguchi's lcrit must be more than 0").arg(where));
        } else if (mob == QLatin1String("expression")) {
            m.mobility = M::Expression;
            for (int c = 0; c < 2; ++c) {
                const QString t = mf->text(c == 0 ? QStringLiteral("muexpn") : QStringLiteral("muexpp"));
                m.muExpr[c] = Expression::compile(t.trimmed().isEmpty() ? QStringLiteral("0") : t, a_exprScope);
                if (!m.muExpr[c].isValid()) problem(tr("%1: its mobility's expression: %2").arg(where, m.muExpr[c].error()));
                else {
                    for (int slot : {a_slotn, a_slotp, a_slotEpar, a_slotEperp, a_slotD}) m.exprState = m.exprState || m.muExpr[c].uses(slot);
                    m.exprAcross = m.exprAcross || m.muExpr[c].uses(a_slotEperp) || m.muExpr[c].uses(a_slotD);
                }
            }
        } else if (!mob.isEmpty() && mob != QLatin1String("constant")) {
            problem(tr("%1: a mobility model %2 is not taken in 3D - constant, by doping (Caughey and Thomas, Masetti), Arora, Dorkel and Leturcq, "
                       "Klaassen, Yamaguchi or an expression is")
                        .arg(where, mob));
        }
        if (mf && a_TLexpr.isValid()) {
            m.zetan = v(QStringLiteral("zetan"), Dim(), tr("the mobilities' temperature exponents"));
            m.zetap = v(QStringLiteral("zetap"), Dim(), tr("the mobilities' temperature exponents"));
            if (!std::isfinite(m.zetan) || !std::isfinite(m.zetap)) m.zetan = m.zetap = 0;
        }
        if (mf && mf->flag(QStringLiteral("saturation"))) {
            m.saturation = true;
            const QString sat = tr("saturation");
            m.vsatn = v(QStringLiteral("vsatn"), velocity, sat);
            m.vsatp = v(QStringLiteral("vsatp"), velocity, sat);
            m.betan = v(QStringLiteral("betan"), Dim(), sat);
            m.betap = v(QStringLiteral("betap"), Dim(), sat);
            m.vsatAn = v(QStringLiteral("Avn"), Dim(), sat);
            m.vsatAp = v(QStringLiteral("Avp"), Dim(), sat);
            m.vsatExp = mf->text(QStringLiteral("vsatmodel")) == QLatin1String("exponential");
            if (!(m.betan > 0) || !(m.betap > 0)) problem(tr("%1: its saturation exponents β must be more than 0").arg(where));
            m.wholeField = mf->text(QStringLiteral("driving")) == QLatin1String("field");
            if (mf->text(QStringLiteral("driving")) == QLatin1String("energy")) {
                m.energy = true;
                m.tauWn = v(QStringLiteral("tauwn"), Dim::of(0, 0, 1), sat);
                m.tauWp = v(QStringLiteral("tauwp"), Dim::of(0, 0, 1), sat);
                if (!(m.tauWn > 0) || !(m.tauWp > 0)) problem(tr("%1: its energy relaxation times must be more than 0").arg(where));
            }
            if (mf->text(QStringLiteral("velocity")) == QLatin1String("transferred")) {
                m.transferred = true;
                m.E0n = v(QStringLiteral("E0n"), Dim::of(1, 1, -3, -1), sat);
                if (!(m.E0n > 0)) problem(tr("%1: its transferred electrons' field E0 must be more than 0").arg(where));
            }
        }
        if (mf && mf->flag(QStringLiteral("surface")) && mf->text(QStringLiteral("surfmodel")) == QLatin1String("shirahata")) {
            m.surface = m.shirahata = true;
            const QString sh = tr("Shirahata's surface scattering");
            const Dim field = Dim::of(1, 1, -3, -1);
            const char* keys[2][6] = {{"shmu0n", "shthetan", "she1n", "she2n", "shp1n", "shp2n"}, {"shmu0p", "shthetap", "she1p", "she2p", "shp1p", "shp2p"}};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 6; ++k) m.sh[c][k] = v(QString::fromLatin1(keys[c][k]), k == 0 ? mobility : k == 2 || k == 3 ? field : Dim(), sh);
            m.lcrit = v(QStringLiteral("lcrit"), Dim::length(), sh);
            for (int c = 0; c < 2; ++c)
                if (!(m.sh[c][0] > 0) || !std::isfinite(m.sh[c][1]) || !(m.sh[c][2] > 0) || !(m.sh[c][3] > 0) || !(m.sh[c][4] > 0) || !(m.sh[c][5] > 0))
                    problem(tr("%1: Shirahata's µ0, E1, E2, P1 and P2 must be more than 0").arg(where));
            if (!(m.lcrit > 0)) problem(tr("%1: its surface scattering's lcrit must be more than 0").arg(where));
        } else if (mf && mf->flag(QStringLiteral("surface"))) {
            m.surface = true;
            const QString lo = tr("Lombardi's surface scattering");
            const Dim perSecond = Dim::voltage() / Dim::of(0, 0, 1);
            const char* keys[2][4] = {{"Bn", "Cn", "lambdan", "deltan"}, {"Bp", "Cp", "lambdap", "deltap"}};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 4; ++k) m.sl[c][k] = v(QString::fromLatin1(keys[c][k]), k == 0 ? velocity : k == 3 ? perSecond : Dim(), lo);
            m.lcrit = v(QStringLiteral("lcrit"), Dim::length(), lo);
            for (int c = 0; c < 2; ++c)
                if (!(m.sl[c][0] > 0) || !(m.sl[c][1] > 0) || !(m.sl[c][3] > 0)) problem(tr("%1: its surface scattering's B, C, δ and lcrit must be more than 0").arg(where));
            if (!(m.lcrit > 0)) problem(tr("%1: its surface scattering's B, C, δ and lcrit must be more than 0").arg(where));
        }
        if (hall()) {
            const QString h = tr("Hall factors");
            m.rHn = mf && !mf->text(QStringLiteral("rhalln")).trimmed().isEmpty() ? v(QStringLiteral("rhalln"), Dim(), h) : 1.0;
            m.rHp = mf && !mf->text(QStringLiteral("rhallp")).trimmed().isEmpty() ? v(QStringLiteral("rhallp"), Dim(), h) : 1.0;
            if (!(m.rHn >= 0) || !(m.rHp >= 0)) problem(tr("%1: its Hall factors must be at least 0").arg(where));
        }
        if (mf && mf->flag(QStringLiteral("fermi"))) m.fermi = true;
        if (mf && mf->flag(QStringLiteral("bgn"))) {
            m.bgn = true;
            m.bgnE = energy(mf->text(QStringLiteral("Ebgn")), where);
            m.bgnN = v(QStringLiteral("Nbgn"), conc, tr("band gap narrowing"));
            m.bgnC = v(QStringLiteral("Cbgn"), Dim(), tr("band gap narrowing"));
        }
        if (mf && mf->flag(QStringLiteral("incomplete"))) {
            m.incomplete = true;
            const double Ed = energy(mf->text(QStringLiteral("Ed")), where), Ea = energy(mf->text(QStringLiteral("Ea")), where);
            m.gd = v(QStringLiteral("gd"), Dim(), tr("its dopants' levels"));
            m.ga = v(QStringLiteral("ga"), Dim(), tr("its dopants' levels"));
            m.n1d = m.Nc * std::exp(-Ed / (KB * a_T));
            m.p1a = m.Nv * std::exp(-Ea / (KB * a_T));
        }
        if (const Node* s = srhOf[std::size_t(d)]) {
            m.srh = true;
            const auto lifetime = [&](const QString& key) {
                const QString t = s->text(key).trimmed().isEmpty() && mat ? detail::Problem::materialText(*mat, key, &params, a_T) : s->text(key);
                return compile(t, *s, key);
            };
            m.taunOf = lifetime(QStringLiteral("taun"));
            m.taupOf = lifetime(QStringLiteral("taup"));
            m.Et = s->text(QStringLiteral("Et")).trimmed().isEmpty() ? 0.0 : energy(s->text(QStringLiteral("Et")), s->name());
            if (s->text(QStringLiteral("dependence")) == QLatin1String("doping")) {
                m.fossum = true;
                const QString fossum = tr("Fossum's doping dependence");
                const auto sv = [&](const QString& key, const Dim& dim) { return number(s->text(key), dim, tr("%1: %2").arg(s->name(), key)); };
                m.N0n = sv(QStringLiteral("N0n"), conc);
                m.N0p = sv(QStringLiteral("N0p"), conc);
                m.fA = sv(QStringLiteral("fA"), Dim());
                m.fB = sv(QStringLiteral("fB"), Dim());
                m.fC = sv(QStringLiteral("fC"), Dim());
                m.fE = sv(QStringLiteral("fE"), Dim());
                (void)fossum;
            }
            if (s->flag(QStringLiteral("tat"))) {
                const double kT = KB * a_T;
                const double mtn = number(s->text(QStringLiteral("mtn")), Dim(), s->name()), mtp = number(s->text(QStringLiteral("mtp")), Dim(), s->name());
                if (!(mtn > 0) || !(mtp > 0)) problem(tr("%1: its tunnelling masses must be more than 0").arg(s->name()));
                else {
                    m.tat = true;
                    m.FGn = std::sqrt(24 * mtn * Mass0 * kT * kT * kT) / (Q * HbarSI);
                    m.FGp = std::sqrt(24 * mtp * Mass0 * kT * kT * kT) / (Q * HbarSI);
                }
            }
        }
        if (const Node* a = augerOf[std::size_t(d)]) {
            m.Cn = number(a->text(QStringLiteral("Cn")), Dim::of(6, 0, -1), a->name());
            m.Cp = number(a->text(QStringLiteral("Cp")), Dim::of(6, 0, -1), a->name());
        }
        if (const Node* r = directOf[std::size_t(d)]) m.B = number(r->text(QStringLiteral("B")), Dim::of(3, 0, -1), r->name());
        if (const Node* bt = btbtOf[std::size_t(d)]) {
            // A in cm^-1/2 V^-5/2 s^-1 for γ 2.5, scaled to SI for E in V/m
            // (the 2D solver's).
            m.tunnelling = true;
            m.btGeneration = bt->text(QStringLiteral("kind")) == QLatin1String("generation");
            m.btGamma = number(bt->text(QStringLiteral("gamma")), Dim(), bt->name());
            const double Acm = number(bt->text(QStringLiteral("A")), Dim(), bt->name());
            m.btB = number(bt->text(QStringLiteral("B")), Dim::of(1, 1, -3, -1), bt->name());
            if (!(Acm > 0) || !(m.btB > 0) || !std::isfinite(m.btGamma)) problem(tr("%1: its A and B must be more than 0").arg(bt->name()));
            m.btA = Acm * 1e6 * std::pow(1e-2, m.btGamma);
        }
        if (const Node* ii = impactOf[std::size_t(d)]; ii && ii->text(QStringLiteral("model")) != QLatin1String("user")) {
            // Silicon's, van Overstraeten and de Man's two ranges (the
            // material's where it is silicon's: the 2D solver's default).
            m.ionization = true;
            m.an = m.an2 = 7.03e7;
            m.bn = m.bn2 = 1.231e8;
            m.ap = 1.582e8;
            m.bp = 2.036e8;
            m.ap2 = 6.71e7;
            m.bp2 = 1.693e8;
            m.E0 = 4e7;
            m.ionPhonon = 0.063 * Q;
        } else if (const Node* ii = impactOf[std::size_t(d)]) {
            m.ionization = true;
            const Dim perM = Dim::of(-1), field = Dim::of(1, 1, -3, -1);
            const auto iv = [&](const QString& key, const Dim& dim) { return number(ii->text(key), dim, tr("%1: %2").arg(ii->name(), key)); };
            m.an = iv(QStringLiteral("an"), perM);
            m.bn = iv(QStringLiteral("bn"), field);
            m.ap = iv(QStringLiteral("ap"), perM);
            m.bp = iv(QStringLiteral("bp"), field);
            const QString e0 = ii->text(QStringLiteral("E0"));
            m.E0 = e0.trimmed().isEmpty() ? Inf : iv(QStringLiteral("E0"), field);
            m.an2 = ii->text(QStringLiteral("an2")).trimmed().isEmpty() ? m.an : iv(QStringLiteral("an2"), perM);
            m.bn2 = ii->text(QStringLiteral("bn2")).trimmed().isEmpty() ? m.bn : iv(QStringLiteral("bn2"), field);
            m.ap2 = ii->text(QStringLiteral("ap2")).trimmed().isEmpty() ? m.ap : iv(QStringLiteral("ap2"), perM);
            m.bp2 = ii->text(QStringLiteral("bp2")).trimmed().isEmpty() ? m.bp : iv(QStringLiteral("bp2"), field);
            m.ionBetaN = ii->text(QStringLiteral("betan")).trimmed().isEmpty() ? 1.0 : iv(QStringLiteral("betan"), Dim());
            m.ionBetaP = ii->text(QStringLiteral("betap")).trimmed().isEmpty() ? 1.0 : iv(QStringLiteral("betap"), Dim());
            m.ionPhonon = ii->text(QStringLiteral("Eph")).trimmed().isEmpty() ? 0.0 : energy(ii->text(QStringLiteral("Eph")), ii->name());
        }
        if (const Node* ii = impactOf[std::size_t(d)]) {
            m.ionCurrents = ii->flag(QStringLiteral("currents"));
            m.ionFermi = ii->text(QStringLiteral("driving")) == QLatin1String("fermi");
            m.ionAlong = ii->text(QStringLiteral("driving")) == QLatin1String("current");
            a_s.log << tr("%1: impact ionization driven by %2%3").arg(where, m.ionFermi ? tr("each carrier's quasi-Fermi gradient") : m.ionAlong ? tr("the field along each current") : tr("the field"),
                                                                       m.ionCurrents ? QString() : tr(" (its integrals alone)"));
        }
        // (For the variables: its constants by name.)
        for (const auto& [key, value] : {std::pair{QStringLiteral("ni"), m.ni}, std::pair{QStringLiteral("dEc"), m.dEc}, std::pair{QStringLiteral("Eg"), m.Eg},
                                         std::pair{QStringLiteral("mun"), m.mun}, std::pair{QStringLiteral("mup"), m.mup}}) {
            auto& vv = coef.named[key];
            vv.resize(D);
            vv[std::size_t(d)] = Expression::compile(QString::number(value, 'g', 17), a_s.pointScope());
        }
    }
    {
        auto& vv = coef.named[QStringLiteral("T")];
        vv.assign(D, Expression::compile(QString::number(a_T, 'g', 17), a_s.pointScope()));
    }
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    // Each semiconductor's bands' class: the first domain's of the same bands
    // (graded: one for all).
    a_bandsOf.assign(D, -1);
    {
        const auto sameBands = [this](int d, int e) {
            const Medium &a = a_media[std::size_t(d)], &b = a_media[std::size_t(e)];
            auto near = [](double u, double v) { return std::abs(u - v) <= 1e-9 * std::max(std::abs(u), std::abs(v)); };
            return near(a.Eg, b.Eg) && near(a.chi + a.dEc, b.chi + b.dEc) && near(a.ni, b.ni);
        };
        for (int d = 0; d < int(D); ++d) {
            if (!a_media[std::size_t(d)].semiconductor) continue;
            a_bandsOf[std::size_t(d)] = d;
            for (int e = 0; e < d; ++e)
                if (a_media[std::size_t(e)].semiconductor && (graded || sameBands(d, e))) {
                    a_bandsOf[std::size_t(d)] = a_bandsOf[std::size_t(e)];
                    break;
                }
        }
    }
    if (!geometry(error)) return false;
    // Doping, by node: each feature's at the node, over its share of the
    // node's box in each of its semiconductor domains (2D's).
    const Mesh3& mesh = a_s.mesh();
    const Space3& sp = *f.space;
    a_Nd.assign(std::size_t(a_n), 0);
    a_Na.assign(std::size_t(a_n), 0);
    a_generation.assign(std::size_t(a_n), 0);
    a_tauN.assign(std::size_t(a_n), Inf);
    a_tauP.assign(std::size_t(a_n), Inf);
    a_genTimed.clear();
    a_genBase.clear();
    a_genPhotonBase.clear();
    a_genGapBase.clear();
    {
        std::vector<double> nd(std::size_t(a_n), 0), na(std::size_t(a_n), 0), g(std::size_t(a_n), 0), vol(std::size_t(a_n), 0);
        std::vector<double> gph(std::size_t(a_n), 0), ggap(std::size_t(a_n), 0);
        a_genPhoton.assign(std::size_t(a_n), 0);
        a_genGapRate.assign(std::size_t(a_n), 0);
        std::vector<std::vector<std::pair<int, double>>> parts(static_cast<std::size_t>(a_n));   // (domain, volume)
        for (int e = 0; e < int(mesh.prisms.size()); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !a_media[std::size_t(d)].semiconductor) continue;
            const auto& pr = mesh.prisms[std::size_t(e)];
            const int j = mesh.prismLayer[std::size_t(e)];
            const double h = (mesh.heights[std::size_t(j) + 1] - mesh.heights[std::size_t(j)]) * mesh.unitScale;
            Vec3 x[3];
            for (int k = 0; k < 3; ++k) x[k] = mesh.nodes[std::size_t(pr[std::size_t(k)])];
            double share[3];
            for (int k = 0; k < 3; ++k) share[k] = cornerShare(x[k], x[(k + 1) % 3], x[(k + 2) % 3], mesh.unitScale);
            for (int k = 0; k < 6; ++k) parts[std::size_t(pr[std::size_t(k)])].emplace_back(d, share[k % 3] * h / 2);
        }
        const auto atPoint = [this](const Expression& e, const Vec3& xi) {
            if (e.isConstant()) return e.constant();
            std::vector<double> values(std::size_t(a_s.pointScope().slotCount()), NaN);
            for (int slot : e.variables()) {
                const auto& def = a_s.definitions()[std::size_t(slot)];
                using K = Solution3::VariableDef::Kind;
                values[std::size_t(slot)] = def.kind == K::X ? xi.x : def.kind == K::Y ? xi.y : def.kind == K::Z ? xi.z : NaN;
            }
            return e.eval(values.data());
        };
        for (int i = 0; i < a_n; ++i) {
            if (parts[std::size_t(i)].empty()) continue;
            const Vec3 xi = sp.point(i);
            double tn = 0, tp = 0, tv = 0;
            for (const auto& [d, vv] : parts[std::size_t(i)]) {
                for (const Doping& dp : dopings) {
                    if (!dp.on[std::size_t(d)]) continue;
                    double N = 0;
                    if (dp.profile == QLatin1String("uniform") || dp.profile == QLatin1String("expression")) N = atPoint(dp.N, xi);
                    else if (dp.profile == QLatin1String("box")) {
                        const double dx = std::max({dp.x0 - xi.x, 0.0, xi.x - dp.x1}), dy = std::max({dp.y0 - xi.y, 0.0, xi.y - dp.y1});
                        const double dz = std::max({dp.z0 - xi.z, 0.0, xi.z - dp.z1});
                        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                        N = dp.sigma > 0 ? dp.N0 * std::exp(-(dist / dp.sigma) * (dist / dp.sigma)) : (dist == 0 ? dp.N0 : 0);
                    } else if (dp.profile == QLatin1String("implant")) {
                        const double depth = dp.zs - xi.z;
                        if (depth < 0 || !(dp.sd > 0)) continue;
                        const double vert = dp.dose / (std::sqrt(2 * M_PI) * dp.sd)
                                            * (std::exp(-(depth - dp.Rp) * (depth - dp.Rp) / (2 * dp.sd * dp.sd)) + std::exp(-(depth + dp.Rp) * (depth + dp.Rp) / (2 * dp.sd * dp.sd)));
                        const double across = 0.5 * ((std::isfinite(dp.w1) ? std::erf((xi.x - dp.w1) / (std::sqrt(2.0) * dp.sl)) : 1.0)
                                                     - (std::isfinite(dp.w2) ? std::erf((xi.x - dp.w2) / (std::sqrt(2.0) * dp.sl)) : -1.0));
                        N = vert * across;
                    }
                    if (!std::isfinite(N) || N < 0) continue;
                    (dp.donor ? nd : na)[std::size_t(i)] += vv * N;
                }
                const Medium& gm = a_media[std::size_t(d)];
                for (std::size_t q = 0; q < gm.generation.size(); ++q) {
                    const Expression& ge = gm.generation[q];
                    const double photon = q < gm.generationPhoton.size() ? gm.generationPhoton[q] : NaN;
                    if (a_s.varies(ge)) {
                        a_genTimed.push_back({i, ge, vv, photon});   // (its share made a fraction below)
                        continue;
                    }
                    const double gv = atPoint(ge, xi);
                    if (!std::isfinite(gv)) continue;
                    g[std::size_t(i)] += vv * gv;
                    if (std::isfinite(photon)) gph[std::size_t(i)] += vv * gv * photon;
                    else ggap[std::size_t(i)] += vv * gv;
                }
                vol[std::size_t(i)] += vv;
            }
            if (vol[std::size_t(i)] != 0) {
                a_Nd[std::size_t(i)] = nd[std::size_t(i)] / vol[std::size_t(i)];
                a_Na[std::size_t(i)] = na[std::size_t(i)] / vol[std::size_t(i)];
                a_generation[std::size_t(i)] = g[std::size_t(i)] / vol[std::size_t(i)];
                a_genPhoton[std::size_t(i)] = gph[std::size_t(i)] / vol[std::size_t(i)];
                a_genGapRate[std::size_t(i)] = ggap[std::size_t(i)] / vol[std::size_t(i)];
            }
            // Its lifetimes: its medium's, of its doping (Fossum's).
            const int d = a_nodeMedium[std::size_t(i)];
            if (d >= 0 && a_media[std::size_t(d)].srh) {
                const Medium& m = a_media[std::size_t(d)];
                tn = atPoint(m.taunOf, xi);
                tp = atPoint(m.taupOf, xi);
                if (m.fossum) {
                    const double N = a_Nd[std::size_t(i)] + a_Na[std::size_t(i)];
                    tn /= m.fA + m.fB * N / m.N0n + m.fC * std::pow(N / m.N0n, m.fE);
                    tp /= m.fA + m.fB * N / m.N0p + m.fC * std::pow(N / m.N0p, m.fE);
                }
                if (!(tn > 0) || !(tp > 0)) {
                    *error = tr("%1: a lifetime at (%2, %3, %4) is not more than 0").arg(a_node->name()).arg(xi.x).arg(xi.y).arg(xi.z);
                    return false;
                }
                a_tauN[std::size_t(i)] = tn;
                a_tauP[std::size_t(i)] = tp;
            }
            (void)tv;
        }
    }
    // (Generation of t: each term's share a fraction of its node's box.)
    {
        std::vector<double> total(std::size_t(a_n), 0);
        for (int e = 0; e < int(mesh.prisms.size()); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !a_media[std::size_t(d)].semiconductor) continue;
            (void)e;
        }
        std::map<int, double> vol;
        for (const TimedGeneration& tg : a_genTimed) vol[tg.node] = 0;
        // (Its box's semiconductor volume, as the doping's.)
        for (TimedGeneration& tg : a_genTimed) tg.share = a_box[std::size_t(tg.node)] > 0 ? tg.share / a_box[std::size_t(tg.node)] : 0.0;
        (void)total;
    }
    a_C0 = 1;
    for (int i = 0; i < a_n; ++i) a_C0 = std::max({a_C0, a_Nd[std::size_t(i)], a_Na[std::size_t(i)]});
    for (const Medium& m : a_media)
        if (m.semiconductor) a_C0 = std::max(a_C0, m.ni);
    // The prisms' doping.
    for (int e = 0; e < int(mesh.prisms.size()); ++e) {
        Prism& pr = a_prisms[std::size_t(e)];
        if (pr.domain < 0) continue;
        pr.Nd = pr.Na = 0;
        for (int k = 0; k < 6; ++k) {
            const int i = mesh.prisms[std::size_t(e)][std::size_t(k)];
            pr.Nd += a_Nd[std::size_t(i)] / 6;
            pr.Na += a_Na[std::size_t(i)] / 6;
        }
    }
    a_TL.assign(std::size_t(a_n), a_T);
    a_field.assign(std::size_t(a_n), 0);
    for (auto* v : {&a_Tn, &a_Tp, &a_TnWas, &a_TpWas, &a_dTnWas, &a_dTpWas}) v->clear();
    a_omegaT[0] = a_omegaT[1] = 1;
    nodeBands();
    mobilities(nullptr);
    f.values.assign(std::size_t(Components * a_n), 0.0);
    for (int i = 0; i < a_n; ++i) {
        f.values[std::size_t(CNd * a_n + i)] = a_Nd[std::size_t(i)];
        f.values[std::size_t(CNa * a_n + i)] = a_Na[std::size_t(i)];
    }
    a_solvedOnce = false;
    return true;
}

bool SemiconductorProblem3::geometry(QString* error)
{
    Field3& f = field();
    const Space3& sp = *f.space;
    const Mesh3& mesh = a_s.mesh();
    a_n = sp.size();
    a_nodeOn.assign(std::size_t(a_n), 0);
    a_nodeMedium.assign(std::size_t(a_n), -1);
    a_box.assign(std::size_t(a_n), 0);
    // (An edge by its nodes and its prisms' bands' class: one along an abrupt
    // heterojunction is two, each its own side's; an insulator's, -1.)
    std::map<std::tuple<int, int, int>, int> index;
    a_edges.clear();
    a_parts.clear();
    a_prisms.assign(mesh.prisms.size(), Prism());
    a_nodeParts.assign(std::size_t(a_n), {});
    const auto edge = [&](int a, int b, int cls, int medium) -> int {
        const auto key = std::make_tuple(std::min(a, b), std::max(a, b), cls);
        auto it = index.find(key);
        if (it == index.end()) {
            it = index.emplace(key, int(a_edges.size())).first;
            Edge e;
            e.i = std::min(a, b);
            e.j = std::max(a, b);
            e.medium = medium;
            const Vec3 pa = sp.point(e.i), pb = sp.point(e.j);
            e.length = std::sqrt((pa.x - pb.x) * (pa.x - pb.x) + (pa.y - pb.y) * (pa.y - pb.y) + (pa.z - pb.z) * (pa.z - pb.z));
            if (e.length > 0) e.t = Vec3{(pb.x - pa.x) / e.length, (pb.y - pa.y) / e.length, (pb.z - pa.z) / e.length};
            a_edges.push_back(e);
        }
        return it->second;
    };
    const double s = mesh.unitScale;
    for (int e = 0; e < int(mesh.prisms.size()); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        const Medium& m = a_media[std::size_t(d)];
        const auto& pr = mesh.prisms[std::size_t(e)];
        const int j = mesh.prismLayer[std::size_t(e)];
        const double h = (mesh.heights[std::size_t(j) + 1] - mesh.heights[std::size_t(j)]) * s;
        const double eps = Eps0 * m.epsr;
        Vec3 x[3];
        for (int k = 0; k < 3; ++k) x[k] = mesh.nodes[std::size_t(pr[std::size_t(k)])];
        if (m.semiconductor) {
            Prism& pm = a_prisms[std::size_t(e)];
            pm.domain = d;
            pm.volume = std::abs((x[1].x - x[0].x) * (x[2].y - x[0].y) - (x[2].x - x[0].x) * (x[1].y - x[0].y)) / 2 * s * s * h;
            // (Its shape functions' gradients at its middle: its field, the
            // forces along its currents.)
            PrismShape ps;
            prismShapeAt(1, 1.0 / 3, 1.0 / 3, 0.5, ps);
            const Space3::Map mp = sp.map(e, 1.0 / 3, 1.0 / 3, 0.5);
            for (int k = 0; k < 6; ++k) Space3::gradient(mp, ps.dxi[k], ps.deta[k], ps.dzeta[k], pm.grad[k]);
        }
        for (int k = 0; k < 6; ++k) {
            const int i = pr[std::size_t(k)];
            a_nodeOn[std::size_t(i)] = 1;
            if (m.semiconductor) {
                // (A heterojunction's node: the narrower gap's - graded
                // over the wider one's first element, as 2D's graded one -,
                // between equal gaps the lower domain's, whatever the order
                // of its prisms: 2D's.)
                const int prev = a_nodeMedium[std::size_t(i)];
                const double g = m.Eg, hh = prev < 0 ? 0.0 : a_media[std::size_t(prev)].Eg;
                if (prev < 0 || g < hh - 1e-9 * std::abs(hh) || (std::abs(g - hh) <= 1e-9 * std::abs(hh) && d < prev)) a_nodeMedium[std::size_t(i)] = d;
            }
        }
        // A horizontal edge's coupling: half the cotangent of the angle
        // opposite it, its dual face's height half the layer's; a vertical
        // one's, its corner's share of the triangle (cornerShare) over the
        // height. A node's box, that share times half the height.
        for (int k = 0; k < 3; ++k) {
            const Vec3& A = x[k];
            const Vec3& B = x[(k + 1) % 3];
            const Vec3& C = x[(k + 2) % 3];
            const double ux = (A.x - C.x) * s, uy = (A.y - C.y) * s, vx = (B.x - C.x) * s, vy = (B.y - C.y) * s;
            const double c = 0.5 * (ux * vx + uy * vy) / std::abs(ux * vy - uy * vx) * h / 2;
            for (int level : {0, 3}) {
                const int ed = edge(pr[std::size_t(k + level)], pr[std::size_t((k + 1) % 3 + level)], m.semiconductor ? a_bandsOf[std::size_t(d)] : -1, m.semiconductor ? d : -1);
                a_edges[std::size_t(ed)].eps += eps * c;
                if (m.semiconductor) {
                    a_parts.push_back({ed, e, c});
                    a_edges[std::size_t(ed)].domain = d;
                    Prism& pp = a_prisms[std::size_t(e)];
                    if (pp.edgeCount < 9) {
                        pp.edges[std::size_t(pp.edgeCount)] = ed;
                        pp.coupling[std::size_t(pp.edgeCount++)] = c;
                    }
                }
            }
        }
        for (int k = 0; k < 3; ++k) {
            const double share = cornerShare(x[k], x[(k + 1) % 3], x[(k + 2) % 3], s);
            const int ed = edge(pr[std::size_t(k)], pr[std::size_t(k + 3)], m.semiconductor ? a_bandsOf[std::size_t(d)] : -1, m.semiconductor ? d : -1);
            a_edges[std::size_t(ed)].eps += eps * share / h;
            if (m.semiconductor) {
                a_parts.push_back({ed, e, share / h});
                a_edges[std::size_t(ed)].domain = d;
                Prism& pp = a_prisms[std::size_t(e)];
                if (pp.edgeCount < 9) {
                    pp.edges[std::size_t(pp.edgeCount)] = ed;
                    pp.coupling[std::size_t(pp.edgeCount++)] = share / h;
                }
                a_box[std::size_t(pr[std::size_t(k)])] += share * h / 2;
                a_box[std::size_t(pr[std::size_t(k + 3)])] += share * h / 2;
                for (int level : {0, 3}) {
                    auto& parts = a_nodeParts[std::size_t(pr[std::size_t(k + level)])];
                    auto at = std::find_if(parts.begin(), parts.end(), [d](const std::pair<int, double>& q) { return q.first == d; });
                    if (at == parts.end()) parts.emplace_back(d, share * h / 2);
                    else at->second += share * h / 2;
                }
            }
        }
    }
    // Where each node recombines: the parts of its box on its own bands - a
    // heterojunction's node not over the other side's part (2D's).
    a_recBox.assign(std::size_t(a_n), 0);
    {
        const auto sameBands = [this](int d, int e) {
            const Medium &a = a_media[std::size_t(d)], &b = a_media[std::size_t(e)];
            auto near = [](double u, double v) { return std::abs(u - v) <= 1e-9 * std::max(std::abs(u), std::abs(v)); };
            return near(a.Eg, b.Eg) && near(a.chi + a.dEc, b.chi + b.dEc) && near(a.ni, b.ni);
        };
        for (int e = 0; e < int(mesh.prisms.size()); ++e) {
            const int d = a_prisms[std::size_t(e)].domain;
            if (d < 0) continue;
            const auto& pr = mesh.prisms[std::size_t(e)];
            const int j = mesh.prismLayer[std::size_t(e)];
            const double h = (mesh.heights[std::size_t(j) + 1] - mesh.heights[std::size_t(j)]) * s;
            Vec3 x[3];
            for (int k = 0; k < 3; ++k) x[k] = mesh.nodes[std::size_t(pr[std::size_t(k)])];
            for (int k = 0; k < 3; ++k) {
                const double share = cornerShare(x[k], x[(k + 1) % 3], x[(k + 2) % 3], s);
                for (int level : {0, 3}) {
                    const int i = pr[std::size_t(k + level)];
                    const int own = a_nodeMedium[std::size_t(i)];
                    if (own >= 0 && sameBands(d, own)) a_recBox[std::size_t(i)] += share * h / 2;
                }
            }
        }
    }
    // Heterojunctions' emission: each edge from a node of the other bands
    // into its own - its face's share across the interface, |t·n| (n the
    // interface's normal at the node, its side's prisms' faces on it), its
    // side's Richardson velocities and masses (of its densities of states:
    // Nc = 2 (2π m* kT/h²)^(3/2)); its steps with the bands (heteroSteps).
    for (Edge& ed : a_edges) ed.emitsAtI = false, ed.across = ed.vRn = ed.vRp = ed.massN = ed.massP = 0;
    if (a_emission > 0) {
        const auto classOf = [this](int i) {
            const int d = a_nodeMedium[std::size_t(i)];
            return d < 0 ? -2 : a_bandsOf[std::size_t(d)];
        };
        std::map<std::pair<int, int>, Vec3> normals;   // (node, class): Σ area normal into it
        static const int faces[5][4] = {{0, 1, 2, -1}, {3, 4, 5, -1}, {0, 1, 4, 3}, {1, 2, 5, 4}, {2, 0, 3, 5}};
        for (int e = 0; e < int(mesh.prisms.size()); ++e) {
            const int d = a_prisms[std::size_t(e)].domain;
            if (d < 0) continue;
            const int C = a_bandsOf[std::size_t(d)];
            const auto& pr = mesh.prisms[std::size_t(e)];
            Vec3 mid{0, 0, 0};
            for (int k = 0; k < 6; ++k) {
                const Vec3 q = sp.point(pr[std::size_t(k)]);
                mid = Vec3{mid.x + q.x / 6, mid.y + q.y / 6, mid.z + q.z / 6};
            }
            for (const auto& fc : faces) {
                const int corners = fc[3] < 0 ? 3 : 4;
                bool onInterface = true;
                for (int k = 0; k < corners && onInterface; ++k) {
                    const int c = classOf(pr[std::size_t(fc[k])]);
                    onInterface = c >= 0 && c != C;
                }
                if (!onInterface) continue;
                Vec3 q[4];
                Vec3 fm{0, 0, 0};
                for (int k = 0; k < corners; ++k) {
                    q[k] = sp.point(pr[std::size_t(fc[k])]);
                    fm = Vec3{fm.x + q[k].x / corners, fm.y + q[k].y / corners, fm.z + q[k].z / corners};
                }
                const Vec3 u = corners == 4 ? Vec3{q[2].x - q[0].x, q[2].y - q[0].y, q[2].z - q[0].z} : Vec3{q[1].x - q[0].x, q[1].y - q[0].y, q[1].z - q[0].z};
                const Vec3 v = corners == 4 ? Vec3{q[3].x - q[1].x, q[3].y - q[1].y, q[3].z - q[1].z} : Vec3{q[2].x - q[0].x, q[2].y - q[0].y, q[2].z - q[0].z};
                Vec3 n{(u.y * v.z - u.z * v.y) / 2, (u.z * v.x - u.x * v.z) / 2, (u.x * v.y - u.y * v.x) / 2};
                if (n.x * (mid.x - fm.x) + n.y * (mid.y - fm.y) + n.z * (mid.z - fm.z) < 0) n = Vec3{-n.x, -n.y, -n.z};
                for (int k = 0; k < corners; ++k) {
                    Vec3& acc = normals[{pr[std::size_t(fc[k])], C}];
                    acc = Vec3{acc.x + n.x, acc.y + n.y, acc.z + n.z};
                }
            }
        }
        const double kT = KB * a_T, hP = 6.62607015e-34;
        const auto massOf = [kT, hP](double N) { return std::pow(N / 2, 2.0 / 3.0) * hP * hP / (2 * M_PI * kT); };
        for (Edge& ed : a_edges) {
            if (ed.medium < 0) continue;
            const int C = a_bandsOf[std::size_t(ed.medium)];
            const bool atI = classOf(ed.i) != C, atJ = classOf(ed.j) != C;
            // (Along the interface, both its ends the other bands': it crosses nothing.)
            if (atI == atJ) continue;
            const auto found = normals.find({atI ? ed.i : ed.j, C});
            if (found == normals.end()) continue;
            const Vec3& n = found->second;
            const double len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (!(len > 0)) continue;
            ed.across = std::abs(ed.t.x * n.x + ed.t.y * n.y + ed.t.z * n.z) / len;
            if (!(ed.across > 1e-6)) {
                ed.across = 0;
                continue;
            }
            ed.emitsAtI = atI;
            const Medium& m = a_media[std::size_t(ed.medium)];
            ed.massN = massOf(m.Nc);
            ed.massP = massOf(m.Nv);
            ed.vRn = std::sqrt(kT / (2 * M_PI * ed.massN));
            ed.vRp = std::sqrt(kT / (2 * M_PI * ed.massP));
        }
    }
    // The contacts' and surfaces' nodes: each face's lumped area (its rule's
    // shape functions).
    const auto lump = [&](std::map<int, double>& nodes) {
        std::map<int, double> out;
        QSet<int> faces;
        for (const auto& [k, v] : nodes) {
            (void)v;
            if (k < 0) faces.insert(-1 - k);
        }
        for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
            if (!faces.contains(mesh.faces[std::size_t(fi)].face)) continue;
            const std::vector<int> fd = sp.faceDofs(fi);
            bool on = !fd.empty();
            for (int dof : fd) on = on && dof >= 0 && a_nodeOn[std::size_t(dof)];
            if (!on) continue;
            // (A floor's corners take their boxes' shares of it, cornerShare's -
            // its charge and recombination over each box's own face, the
            // potential then even across it; a wall's, a quarter each, its
            // boxes' too.)
            const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];
            if (!mf.quad) {
                Vec3 x[3];
                for (int k = 0; k < 3; ++k) x[k] = mesh.nodes[std::size_t(mf.nodes[std::size_t(k)])];
                for (int k = 0; k < 3; ++k) out[mf.nodes[std::size_t(k)]] += cornerShare(x[k], x[(k + 1) % 3], x[(k + 2) % 3], mesh.unitScale);
                continue;
            }
            for (const Space3::FacePoint& fp : sp.facePoints(fi, 2))
                for (std::size_t k = 0; k < fd.size(); ++k) out[fd[k]] += fp.weight * fp.n[k];
        }
        nodes = out;
    };
    a_heldKind.assign(std::size_t(a_n), -1);
    for (int c = 0; c < int(a_contacts.size()); ++c) {
        Contact& ct = a_contacts[std::size_t(c)];
        lump(ct.nodes);
        if (ct.nodes.empty()) {
            *error = tr("%1: contact %2 is on none of its faces").arg(a_node->name(), ct.name);
            return false;
        }
        for (const auto& [i, area] : ct.nodes) {
            (void)area;
            if (ct.kind == Contact::Gate && a_nodeMedium[std::size_t(i)] >= 0) {
                *error = tr("%1: gate contact %2 touches a semiconductor: it is on an insulator's face (an oxide's top)").arg(a_node->name(), ct.name);
                return false;
            }
            if (ct.kind != Contact::Gate && a_nodeMedium[std::size_t(i)] < 0) continue;
            a_heldKind[std::size_t(i)] = c;
        }
    }
    // A surface's traps at a contact: none at a Schottky one (its emission
    // stands) or an ohmic one (its carriers held) - unless its minority
    // carriers alone recombine there, freed (its "Apply to majority
    // carriers" off; refused elsewhere: a sink of one carrier where no
    // contact takes the other).
    a_minorityFree.assign(std::size_t(a_n), 0);
    for (Surface& sf : a_surfaces) {
        lump(sf.nodes);
        if (sf.levels.empty()) continue;
        int elsewhere = 0, dropped = 0;
        for (auto it = sf.nodes.begin(); it != sf.nodes.end();) {
            const int i = it->first, c = a_heldKind[std::size_t(i)];
            const bool ohmic = c >= 0 && a_contacts[std::size_t(c)].kind == Contact::Ohmic;
            if (sf.minorityOnly && !ohmic && a_nodeMedium[std::size_t(i)] >= 0) ++elsewhere;
            if (c >= 0 && !(ohmic && sf.minorityOnly)) {
                it = sf.nodes.erase(it);
                ++dropped;
                continue;
            }
            if (ohmic) a_minorityFree[std::size_t(i)] = 1;
            ++it;
        }
        if (elsewhere > 0) {
            *error = tr("%1: its minority carriers alone (Apply to majority carriers off) at an ohmic contact only - %2 of its nodes are on none: at an "
                        "insulator's or another semiconductor's interface both carriers recombine (on)")
                         .arg(sf.name)
                         .arg(elsewhere);
            return false;
        }
        if (dropped > 0 && !sf.charged && !sf.minorityOnly) {
            const QString w = tr("%1: at its contacts the carriers held at equilibrium, its recombination none there (Apply to majority carriers off: an "
                                 "ohmic contact's minority carriers' by it)")
                                  .arg(sf.name);
            if (!a_s.log.contains(w)) a_s.log << w;
        }
    }
    // Each edge's dual face over its length, its semiconductor prisms'.
    for (const EdgePart& part : a_parts) a_edges[std::size_t(part.edge)].geo += part.c;
    // Surface scattering's: each semiconductor prism's nearest interface
    // with an insulator - its distance from the prism's middle, its normal.
    bool across = false;
    for (const Medium& m : a_media) across = across || (m.semiconductor && m.perpendicular());
    if (across) {
        struct Tri {
            Vec3 a, b, c, n;
        };
        std::vector<Tri> tris;
        const Topology3& tp = a_s.topology();
        for (const Mesh3::Face& mf : mesh.faces) {
            if (mf.face < 0 || mf.face >= int(tp.faces.size())) continue;
            const Topology3::Face& tf = tp.faces[std::size_t(mf.face)];
            bool semi = false, ins = false;
            for (int d : {tf.back, tf.front}) {
                if (d < 0 || d >= int(a_media.size()) || !a_media[std::size_t(d)].on) continue;
                (a_media[std::size_t(d)].semiconductor ? semi : ins) = true;
            }
            if (!semi || !ins) continue;
            Vec3 q[4];
            for (int k = 0; k < (mf.quad ? 4 : 3); ++k) {
                const Vec3 pt = mesh.nodes[std::size_t(mf.nodes[std::size_t(k)])];
                q[k] = Vec3{pt.x * s, pt.y * s, pt.z * s};
            }
            const auto push = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
                const Vec3 u{b.x - a.x, b.y - a.y, b.z - a.z}, w{c.x - a.x, c.y - a.y, c.z - a.z};
                Vec3 n{u.y * w.z - u.z * w.y, u.z * w.x - u.x * w.z, u.x * w.y - u.y * w.x};
                const double l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                if (!(l > 0)) return;
                tris.push_back({a, b, c, Vec3{n.x / l, n.y / l, n.z / l}});
            };
            push(q[0], q[1], q[2]);
            if (mf.quad) push(q[0], q[2], q[3]);
        }
        // (The nearest point of a triangle to p: Ericson's regions.)
        const auto nearest = [](const Vec3& p, const Tri& t) {
            const auto sub = [](const Vec3& u, const Vec3& w) { return Vec3{u.x - w.x, u.y - w.y, u.z - w.z}; };
            const auto dot = [](const Vec3& u, const Vec3& w) { return u.x * w.x + u.y * w.y + u.z * w.z; };
            const auto at = [](const Vec3& o, const Vec3& u, double k) { return Vec3{o.x + k * u.x, o.y + k * u.y, o.z + k * u.z}; };
            const Vec3 ab = sub(t.b, t.a), ac = sub(t.c, t.a), ap = sub(p, t.a);
            const double d1 = dot(ab, ap), d2 = dot(ac, ap);
            if (d1 <= 0 && d2 <= 0) return t.a;
            const Vec3 bp = sub(p, t.b);
            const double d3 = dot(ab, bp), d4 = dot(ac, bp);
            if (d3 >= 0 && d4 <= d3) return t.b;
            const double vc = d1 * d4 - d3 * d2;
            if (vc <= 0 && d1 >= 0 && d3 <= 0) return at(t.a, ab, d1 / (d1 - d3));
            const Vec3 cp = sub(p, t.c);
            const double d5 = dot(ab, cp), d6 = dot(ac, cp);
            if (d6 >= 0 && d5 <= d6) return t.c;
            const double vb = d5 * d2 - d1 * d6;
            if (vb <= 0 && d2 >= 0 && d6 <= 0) return at(t.a, ac, d2 / (d2 - d6));
            const double va = d3 * d6 - d5 * d4;
            if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return at(t.b, sub(t.c, t.b), (d4 - d3) / ((d4 - d3) + (d5 - d6)));
            const double den = 1 / (va + vb + vc);
            return at(at(t.a, ab, vb * den), ac, vc * den);
        };
        for (int e = 0; e < int(a_prisms.size()); ++e) {
            Prism& pm = a_prisms[std::size_t(e)];
            if (pm.domain < 0 || !a_media[std::size_t(pm.domain)].perpendicular()) continue;
            Vec3 mid{0, 0, 0};
            for (int k = 0; k < 6; ++k) {
                const Vec3 pt = sp.point(mesh.prisms[std::size_t(e)][std::size_t(k)]);
                mid = Vec3{mid.x + pt.x / 6, mid.y + pt.y / 6, mid.z + pt.z / 6};
            }
            for (const Tri& t : tris) {
                const Vec3 q = nearest(mid, t);
                const double d = std::sqrt((q.x - mid.x) * (q.x - mid.x) + (q.y - mid.y) * (q.y - mid.y) + (q.z - mid.z) * (q.z - mid.z));
                if (d < pm.dist) {
                    pm.dist = d;
                    pm.nrm[0] = t.n.x;
                    pm.nrm[1] = t.n.y;
                    pm.nrm[2] = t.n.z;
                }
            }
        }
    }
    // Fowler-Nordheim's faces: each mesh face of them between an insulator
    // and a semiconductor - its middle, its area, its normal (turned into
    // the insulator when the field is walked).
    const Topology3& topo = a_s.topology();
    for (FowlerNordheim& fn : a_fn) {
        for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
            const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];
            if (!fn.faces.count(mf.face) || mf.face < 0 || mf.face >= int(topo.faces.size())) continue;
            const Topology3::Face& tf = topo.faces[std::size_t(mf.face)];
            int ins = -1, semi = -1;
            for (int d : {tf.back, tf.front}) {
                if (d < 0 || d >= int(a_media.size()) || !a_media[std::size_t(d)].on) continue;
                (a_media[std::size_t(d)].semiconductor ? semi : ins) = d;
            }
            if (ins < 0 || semi < 0) continue;
            const int corners = mf.quad ? 4 : 3;
            Vec3 x[4];
            Vec3 mid{0, 0, 0};
            for (int k = 0; k < corners; ++k) {
                const Vec3 p = mesh.nodes[std::size_t(mf.nodes[std::size_t(k)])];
                x[k] = Vec3{p.x * mesh.unitScale, p.y * mesh.unitScale, p.z * mesh.unitScale};
                mid = Vec3{mid.x + x[k].x / corners, mid.y + x[k].y / corners, mid.z + x[k].z / corners};
            }
            // (Its normal and area: a triangle's cross product, a quad's of
            // its diagonals.)
            const Vec3 u = mf.quad ? Vec3{x[2].x - x[0].x, x[2].y - x[0].y, x[2].z - x[0].z} : Vec3{x[1].x - x[0].x, x[1].y - x[0].y, x[1].z - x[0].z};
            const Vec3 v = mf.quad ? Vec3{x[3].x - x[1].x, x[3].y - x[1].y, x[3].z - x[1].z} : Vec3{x[2].x - x[0].x, x[2].y - x[0].y, x[2].z - x[0].z};
            const Vec3 c{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
            const double len = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
            if (!(len > 0)) continue;
            FowlerNordheim::Point pt;
            pt.x = mid;
            pt.n = Vec3{c.x / len, c.y / len, c.z / len};
            pt.area = len / 2;
            pt.insulator = ins;
            fn.points.push_back(pt);
        }
        if (fn.points.empty()) {
            *error = tr("%1: no insulator meets a semiconductor on a Fowler-Nordheim face").arg(a_node->name());
            return false;
        }
    }
    // (Each circuit's contact's own voltage: an unknown after the nodes'.)
    a_unknowns = 3 * a_n;
    for (Contact& c : a_contacts) c.iC = c.circuit != Contact::Held ? a_unknowns++ : -1;
    bool held = false;
    for (int k : a_heldKind) held = held || k >= 0;
    if (!held) {
        *error = tr("%1 has no contact: a Metal Contact, a Schottky or a Gate Contact is needed").arg(a_node->name());
        return false;
    }
    return true;
}

// ----------------------------------------------------------------- Bands

void SemiconductorProblem3::nodeBands()
{
    // Each node's carriers' prefactors against the reference semiconductor's
    // intrinsic level at V = 0 (the Fermi level's potential φ): n = Nc
    // e^((qV + χ - qφ - χr - dEcr)/kT), p = Nv e^((qφ + χr + dEcr - qV - χ -
    // Eg)/kT); its narrowing (by doping, Slotboom's, and from T to its
    // lattice's TL) half from each band.
    a_an.assign(std::size_t(a_n), 0);
    a_ap.assign(std::size_t(a_n), 0);
    a_gap.assign(std::size_t(a_n), 0);
    for (int i = 0; i < a_n; ++i) {
        const int d = a_nodeMedium[std::size_t(i)];
        if (d < 0) continue;
        bandsIn(d, i, &a_an[std::size_t(i)], &a_ap[std::size_t(i)], &a_gap[std::size_t(i)]);
    }
    a_neutralU.assign(std::size_t(a_n), 0);
    for (int i = 0; i < a_n; ++i)
        if (a_nodeMedium[std::size_t(i)] >= 0) a_neutralU[std::size_t(i)] = neutralU(i);
    heteroSteps();
}

void SemiconductorProblem3::bandsIn(int d, int i, double* an, double* ap, double* gap) const
{
    const double kT = KB * a_T;
    const Medium& m = a_media[std::size_t(d)];
    double g = 0;
    const double N = a_Nd[std::size_t(i)] + a_Na[std::size_t(i)];
    if (m.bgn && N > 0) {
        const double l = std::log(N / m.bgnN);
        g = m.bgnE * (l + std::sqrt(l * l + m.bgnC));
    }
    // (Its lattice's TL: its intrinsic density its material's there,
    // 2kT ln(ni(TL)/ni(T)) - the carriers' kT the physics' T.)
    if (!m.lnNiAt.empty() && std::abs(a_TL[std::size_t(i)] - a_T) > 1e-9) g += 2 * kT * (detail::tabulated(m.lnNiAt, a_TL[std::size_t(i)]) - std::log(m.ni));
    if (gap) *gap = g;
    *an = std::log(m.Nc) + (m.chi - a_refChi - a_refDEc) / kT + g / (2 * kT);
    *ap = std::log(m.Nv) + (a_refChi + a_refDEc - m.chi - m.Eg) / kT + g / (2 * kT);
}

void SemiconductorProblem3::heteroSteps()
{
    // An edge on another semiconductor's bands than an end's node: its
    // carriers that one's at the node, the step from the node's bands to its
    // (abrupt); else the node's own.
    for (Edge& e : a_edges) {
        e.anI = e.anJ = e.apI = e.apJ = 0;
        e.fNi = e.fNj = e.fPi = e.fPj = 1;
        if (a_nodeMedium[std::size_t(e.i)] < 0 || a_nodeMedium[std::size_t(e.j)] < 0) continue;
        for (const auto& [node, an, ap, fN, fP] : {std::tuple{e.i, &e.anI, &e.apI, &e.fNi, &e.fPi}, std::tuple{e.j, &e.anJ, &e.apJ, &e.fNj, &e.fPj}}) {
            const std::size_t k = std::size_t(node);
            *an = a_an[k];
            *ap = a_ap[k];
            const int own = a_nodeMedium[k];
            if (e.medium < 0 || a_bandsOf.empty() || a_bandsOf[std::size_t(own)] == a_bandsOf[std::size_t(e.medium)]) continue;
            bandsIn(e.medium, node, an, ap, nullptr);
            *fN = std::exp(*an - a_an[k]);
            *fP = std::exp(*ap - a_ap[k]);
        }
        // (An emitting edge's steps at its end on the interface.)
        e.stepN = e.stepP = 0;
        if (e.across > 0) {
            const double kT = KB * a_T;
            e.stepN = kT * std::abs(e.emitsAtI ? e.anI - a_an[std::size_t(e.i)] : e.anJ - a_an[std::size_t(e.j)]);
            e.stepP = kT * std::abs(e.emitsAtI ? e.apI - a_ap[std::size_t(e.i)] : e.apJ - a_ap[std::size_t(e.j)]);
        }
    }
    // Each node's box for its carriers: its own bands' parts, and the other
    // sides' parts on theirs (its density there times e^(an' - an)).
    a_boxN = a_box;
    a_boxP = a_box;
    if (a_nodeParts.size() != std::size_t(a_n) || a_bandsOf.empty()) return;
    for (int i = 0; i < a_n; ++i) {
        const int own = a_nodeMedium[std::size_t(i)];
        if (own < 0) continue;
        double bn = 0, bp = 0;
        for (const auto& [d, vol] : a_nodeParts[std::size_t(i)]) {
            if (a_bandsOf[std::size_t(d)] == a_bandsOf[std::size_t(own)]) {
                bn += vol;
                bp += vol;
                continue;
            }
            double an = 0, ap = 0;
            bandsIn(d, i, &an, &ap, nullptr);
            bn += vol * std::exp(an - a_an[std::size_t(i)]);
            bp += vol * std::exp(ap - a_ap[std::size_t(i)]);
        }
        a_boxN[std::size_t(i)] = bn;
        a_boxP[std::size_t(i)] = bp;
    }
}

void SemiconductorProblem3::carriersAt(int i, double u, double* n, double* p, double* dn, double* dp) const
{
    const int d = a_nodeMedium[std::size_t(i)];
    const Medium& m = a_media[std::size_t(d)];
    if (m.fermi) {
        // Fermi-Dirac's: Nc F½(η), η its Boltzmann exponent less ln Nc.
        const auto& fh = detail::fermiHalf();
        double F = 0, dF = 0;
        const double etaN = u / a_Vt + a_an[std::size_t(i)] - std::log(m.Nc);
        fh.at(etaN, &F, &dF);
        *n = m.Nc * F;
        *dn = m.Nc * dF / a_Vt;
        const double etaP = -u / a_Vt + a_ap[std::size_t(i)] - std::log(m.Nv);
        fh.at(etaP, &F, &dF);
        *p = m.Nv * F;
        *dp = -m.Nv * dF / a_Vt;
        return;
    }
    *n = std::exp(u / a_Vt + a_an[std::size_t(i)]);
    *p = std::exp(-u / a_Vt + a_ap[std::size_t(i)]);
    *dn = *n / a_Vt;
    *dp = -*p / a_Vt;
}

void SemiconductorProblem3::ionized(int i, double n, double p, double* nd, double* dnd, double* na, double* dna) const
{
    const Medium& m = a_media[std::size_t(a_nodeMedium[std::size_t(i)])];
    const double Nd = a_Nd[std::size_t(i)], Na = a_Na[std::size_t(i)];
    if (!m.incomplete) {
        *nd = Nd;
        *na = Na;
        *dnd = *dna = 0;
        return;
    }
    // Nd⁺ = Nd / (1 + gd n/n1), Na⁻ = Na / (1 + ga p/p1).
    const double fd = 1 + m.gd * n / m.n1d, fa = 1 + m.ga * p / m.p1a;
    *nd = Nd / fd;
    *dnd = -Nd * m.gd / m.n1d / (fd * fd);
    *na = Na / fa;
    *dna = -Na * m.ga / m.p1a / (fa * fa);
}

double SemiconductorProblem3::neutralU(int i) const
{
    // p - n + Nd⁺ - Na⁻ = 0 in u, bisected (it falls as u rises).
    const auto charge = [&](double u) {
        double n, p, dn, dp, nd, dnd, na, dna;
        carriersAt(i, u, &n, &p, &dn, &dp);
        ionized(i, n, p, &nd, &dnd, &na, &dna);
        return p - n + nd - na;
    };
    double lo = -1, hi = 1;
    for (int k = 0; k < 60 && charge(lo) < 0; ++k) lo -= 1;
    for (int k = 0; k < 60 && charge(hi) > 0; ++k) hi += 1;
    for (int k = 0; k < 200 && hi - lo > 1e-13; ++k) {
        const double mid = 0.5 * (lo + hi);
        (charge(mid) > 0 ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

// ------------------------------------------------------------ Mobilities

void SemiconductorProblem3::mobilities(const Eigen::VectorXd* x)
{
    using M = Medium::Mobility;
    const Mesh3& mesh = a_s.mesh();
    const double Vt = a_Vt;
    // (Hot carriers: their temperatures at x, an energy balance's.)
    if (x && hot()) carrierTemperatures(*x);
    // Each carrier's quasi-Fermi potential at a node (Fermi-Dirac's too).
    const auto quasiFermi = [&](int i, double* phin, double* phip) {
        const Medium& m = a_media[std::size_t(a_nodeMedium[std::size_t(i)])];
        const double n = std::max((*x)(iN(i)) * a_C0, 1e-300), p = std::max((*x)(iP(i)) * a_C0, 1e-300);
        double un = Vt * (std::log(n) - a_an[std::size_t(i)]), up = -Vt * (std::log(p) - a_ap[std::size_t(i)]);
        if (m.fermi) {
            const auto& fh = detail::fermiHalf();
            un = Vt * (fh.inverse(n / m.Nc) - a_an[std::size_t(i)] + std::log(m.Nc));
            up = -Vt * (fh.inverse(p / m.Nv) - a_ap[std::size_t(i)] + std::log(m.Nv));
        }
        *phin = (*x)(iV(i)) - un;
        *phip = (*x)(iV(i)) - up;
    };
    for (int e = 0; e < int(a_prisms.size()); ++e) {
        Prism& pr = a_prisms[std::size_t(e)];
        if (pr.domain < 0) continue;
        const Medium& m = a_media[std::size_t(pr.domain)];
        const auto& corners = mesh.prisms[std::size_t(e)];
        const double N = pr.Nd + pr.Na;
        double n = 0, p = 0, TL = 0;
        bool step = false;   // (a corner on another semiconductor's bands: a heterojunction's step in it)
        for (int k = 0; k < 6; ++k) {
            const int i = corners[std::size_t(k)];
            TL += a_TL[std::size_t(i)] / 6;
            const int d = a_nodeMedium[std::size_t(i)];
            step = step || d < 0 || (d != pr.domain && (a_media[std::size_t(d)].Eg != m.Eg || a_media[std::size_t(d)].chi != m.chi));
            if (x && d >= 0) {
                n += (*x)(iN(i)) * a_C0 / 6;
                p += (*x)(iP(i)) * a_C0 / 6;
            }
        }
        if (!x) {
            // (At rest: its dopants' carriers, no field.)
            n = std::max(pr.Nd - pr.Na, 0.0) + m.ni;
            p = std::max(pr.Na - pr.Nd, 0.0) + m.ni;
        }
        // Its field, the field across the nearest interface (faded from it),
        // each carrier's force along its current: its quasi-Fermi
        // potential's gradient, no more than the field.
        double E[3] = {0, 0, 0};
        if (x) fieldIn(e, *x, E);
        const double whole = std::sqrt(E[0] * E[0] + E[1] * E[1] + E[2] * E[2]);
        const bool near = pr.dist < Inf;
        const double perp = near ? std::abs(E[0] * pr.nrm[0] + E[1] * pr.nrm[1] + E[2] * pr.nrm[2]) : 0.0;
        const double damp = near ? std::exp(-pr.dist / m.lcrit) : 0.0;
        double Fn = whole, Fp = whole;
        if (x && !step && ((m.saturation && !m.wholeField) || m.mobility == M::Yamaguchi || m.mobility == M::Expression)) {
            double gn[3] = {0, 0, 0}, gp[3] = {0, 0, 0};
            bool all = true;
            for (int k = 0; k < 6; ++k) {
                const int i = corners[std::size_t(k)];
                if (a_nodeMedium[std::size_t(i)] < 0) {
                    all = false;
                    break;
                }
                double fn = 0, fp = 0;
                quasiFermi(i, &fn, &fp);
                for (int c = 0; c < 3; ++c) {
                    gn[c] += pr.grad[k][c] * fn;
                    gp[c] += pr.grad[k][c] * fp;
                }
            }
            if (all) {
                Fn = std::min(whole, std::sqrt(gn[0] * gn[0] + gn[1] * gn[1] + gn[2] * gn[2]));
                Fp = std::min(whole, std::sqrt(gp[0] * gp[0] + gp[1] * gp[1] + gp[2] * gp[2]));
            }
        }
        if (!x) Fn = Fp = 0;
        double mun = m.mun, mup = m.mup;
        switch (m.mobility) {
        case M::Caughey:
            mun = m.ct[0][0] + (m.mun - m.ct[0][0]) / (1 + std::pow(N / m.ct[0][1], m.ct[0][2]));
            mup = m.ct[1][0] + (m.mup - m.ct[1][0]) / (1 + std::pow(N / m.ct[1][1], m.ct[1][2]));
            break;
        case M::Masetti:
            for (int c = 0; c < 2; ++c) {
                const double* k = m.ms[c];   // µmin1, µmin2, µ1, Pc, Cr, Cs, α, β
                const double mu = k[0] * std::exp(-k[3] / std::max(N, 1.0)) + (m.msMax[c] - k[1]) / (1 + std::pow(N / k[4], k[6])) - k[2] / (1 + std::pow(k[5] / std::max(N, 1.0), k[7]));
                (c == 0 ? mun : mup) = mu;
            }
            break;
        case M::Arora:
            mun = detail::aroraMobility(m.ar[0], N, TL);
            mup = detail::aroraMobility(m.ar[1], N, TL);
            break;
        case M::Dorkel:
            mun = detail::dorkelMobility(m.dl[0], N, n, p, TL);
            mup = detail::dorkelMobility(m.dl[1], N, n, p, TL);
            break;
        case M::Klaassen:
            for (int c = 0; c < 2; ++c) {
                const double* k = m.kl[c];   // µmax, µmin, θ, Nref, α
                const double tn = TL / 300;
                auto cluster = [](double Nx, double cl, double Nref) { return Nx > 0 ? Nx * (1 + 1 / (cl + (Nref / Nx) * (Nref / Nx))) : 0.0; };
                const double nd = cluster(pr.Nd, m.klCD, m.klNrefD), na = cluster(pr.Na, m.klCA, m.klNrefA);
                const double nn = std::max(n, 1e6), pp = std::max(p, 1e6);
                const double mass = c == 0 ? m.klMe : m.klMh, ratio = c == 0 ? m.klMe / m.klMh : m.klMh / m.klMe;
                const double attract = c == 0 ? nd : na, repel = c == 0 ? na : nd, other = c == 0 ? pp : nn;
                const double Nsc = std::max(attract + repel + other, 1e6);
                const double cm3 = Nsc * 1e-6;
                const double Pcw = 3.97e13 / std::cbrt(cm3 * cm3), Pbh = 1.36e20 / ((nn + pp) * 1e-6) * mass;
                const double P = tn * tn / (2.459 / Pcw + 3.828 / Pbh);
                const double G = detail::klaassenG(std::max(P, m.klPmin[c]), TL, mass), Fs = detail::klaassenF(P, ratio);
                const double Neff = attract + G * repel + other / Fs;
                const double muL = k[0] * std::pow(tn, -k[2]);
                const double muN = k[0] * k[0] / (k[0] - k[1]) * std::pow(tn, 3 * k[4] - 1.5);
                const double muC = k[0] * k[1] / (k[0] - k[1]) / std::sqrt(tn);
                const double muDA = muN * (Nsc / Neff) * std::pow(k[3] / Nsc, k[4]) + muC * (nn + pp) / Neff;
                (c == 0 ? mun : mup) = 1 / (1 / muL + 1 / muDA);
            }
            break;
        case M::Yamaguchi: {
            // Its low-field mobility, the field across the interface's
            // (faded from it), the force along the current's: µs = µ0 (1 +
            // as D E⊥)^-½, µ = µs (1 + x²/(x + G) + (µs E∥/vs)²)^-½, x = µs
            // E∥/uL.
            auto yamaguchi = [damp, perp](double mu0, const double* c, double F) {
                const double ms = mu0 / std::sqrt(1 + c[3] * damp * perp);
                const double u = ms * F / c[4], v = ms * F / c[5];
                return ms / std::sqrt(1 + (u > 0 ? u * u / (u + c[6]) : 0.0) + v * v);
            };
            mun = yamaguchi(detail::yamaguchiLowField(m.ya[0], N), m.ya[0], Fn);
            mup = yamaguchi(detail::yamaguchiLowField(m.ya[1], N), m.ya[1], Fp);
            break;
        }
        case M::Expression: {
            std::vector<double> vv(std::size_t(std::max(a_exprScope.slotCount(), 1)), 0.0);
            auto set = [&vv](int slot, double value) {
                if (slot >= 0 && slot < int(vv.size())) vv[std::size_t(slot)] = value;
            };
            set(a_slotN, N);
            set(a_slotNd, pr.Nd);
            set(a_slotNa, pr.Na);
            set(a_slotT, TL);
            set(a_slotn, n);
            set(a_slotp, p);
            set(a_slotEperp, perp);
            set(a_slotD, near ? pr.dist : 1.0);
            set(a_slotEpar, Fn);
            const double en = m.muExpr[0].isValid() ? m.muExpr[0].eval(vv.data()) : 0.0;
            set(a_slotEpar, Fp);
            const double ep = m.muExpr[1].isValid() ? m.muExpr[1].eval(vv.data()) : 0.0;
            mun = std::isfinite(en) && en > 1e-10 ? en : 1e-10;
            mup = std::isfinite(ep) && ep > 1e-10 ? ep : 1e-10;
            break;
        }
        default:
            break;
        }
        // (Its lattice's temperature: (TL/T)^-ζ, where the model has no T of
        // its own.)
        if (std::abs(TL - a_T) > 1e-9 && (m.mobility == M::Constant || m.mobility == M::Caughey || m.mobility == M::Masetti)) {
            mun *= std::pow(TL / a_T, -m.zetan);
            mup *= std::pow(TL / a_T, -m.zetap);
        }
        // (Yamaguchi's on its own: neither a surface's scattering nor
        // saturation's after it.)
        if (m.mobility != M::Yamaguchi && x && m.surface && damp > 0) {
            if (m.shirahata) {
                // 1/µ = 1/µbulk + D (1/µE - 1/µE(0)), µE = µ0 (TL/300 K)^-θ /
                // ((1 + E⊥/E1)^P1 + (E⊥/E2)^P2).
                const double tn = TL / 300;
                auto shirahata = [damp, perp, tn](double mu, const double* c) {
                    const double base = c[0] * std::pow(tn, -c[1]);
                    const double field = base / (std::pow(1 + perp / c[2], c[4]) + std::pow(perp / c[3], c[5]));
                    return 1 / (1 / mu + damp * (1 / field - 1 / base));
                };
                mun = shirahata(mun, m.sh[0]);
                mup = shirahata(mup, m.sh[1]);
            } else {
                // 1/µ = 1/µbulk + D/µac + D/µsr (Matthiessen's rule), µac =
                // B/E⊥ + C (N/1 cm⁻³)^λ / (T E⊥^⅓), µsr = δ/E⊥²; E⊥ no less
                // than 1 V/cm, T the lattice's.
                const double a = std::max(perp, 100.0), perCm3 = std::max(N * 1e-6, 1.0), vPerCm = a / 100;
                auto scatter = [&](double mu, const double* c) {
                    const double ac = c[0] / a + 1e-4 * c[1] * std::pow(perCm3, c[2]) / (TL * std::cbrt(vPerCm));
                    const double sr = c[3] / (a * a);
                    return 1 / (1 / mu + damp / ac + damp / sr);
                };
                mun = scatter(mun, m.sl[0]);
                mup = scatter(mup, m.sl[1]);
            }
        }
        if (m.mobility != M::Yamaguchi && x && m.saturation && !step) {
            // µ(F) = µ0 / (1 + (µ0 F / vsat)^β)^(1/β), F the force along each
            // carrier's current (or the whole field); the transferred
            // electrons' (µ0 + (vsat/F)(F/E0)⁴)/(1 + (F/E0)⁴); hot carriers'
            // the field that heats them so at rest and alone, µ(F) F² =
            // (3/2) k (T - TL)/(q τw).
            const double vsn = m.vsatExp ? detail::expVsat(TL) : m.vsatn * detail::quayVsat(m.vsatAn, TL);
            const double vsp = m.vsatExp ? detail::expVsat(TL) : m.vsatp * detail::quayVsat(m.vsatAp, TL);
            const double mun0 = mun, mup0 = mup;
            auto reduce = [](double mu, double F, double vsat, double beta) { return mu / std::pow(1 + std::pow(mu * F / vsat, beta), 1 / beta); };
            auto electrons = [&](double F) {
                if (!m.transferred) return reduce(mun0, F, vsn, m.betan);
                const double r4 = std::pow(F / m.E0n, 4);
                return F > 0 ? (mun0 + vsn / F * r4) / (1 + r4) : mun0;
            };
            auto holes = [&](double F) { return reduce(mup0, F, vsp, m.betap); };
            auto heated = [](const std::function<double(double)>& mobility, double w) {
                if (!(w > 0)) return 0.0;
                double lo = 0, hi = 1e3;
                while (mobility(hi) * hi * hi < w && hi < 1e12) hi *= 4;
                for (int k = 0; k < 80; ++k) {
                    const double mid = 0.5 * (lo + hi);
                    (mobility(mid) * mid * mid < w ? lo : hi) = mid;
                }
                return 0.5 * (lo + hi);
            };
            if (m.energy && a_Tn.size() == std::size_t(a_n) && a_Tp.size() == std::size_t(a_n)) {
                double Tn = 0, Tp = 0;
                for (int k = 0; k < 6; ++k) {
                    Tn += a_Tn[std::size_t(corners[std::size_t(k)])] / 6;
                    Tp += a_Tp[std::size_t(corners[std::size_t(k)])] / 6;
                }
                Fn = heated(electrons, 1.5 * KB * (Tn - TL) / (Q * m.tauWn));
                Fp = heated(holes, 1.5 * KB * (Tp - TL) / (Q * m.tauWp));
            }
            mun = electrons(Fn);
            mup = holes(Fp);
        }
        pr.mun = mun;
        pr.mup = mup;
    }
    // The edges' weights: each prism's share.
    for (Edge& ed : a_edges) ed.wn = ed.wp = ed.muN = ed.muP = 0;
    for (const EdgePart& part : a_parts) {
        Edge& ed = a_edges[std::size_t(part.edge)];
        const Prism& pr = a_prisms[std::size_t(part.prism)];
        const Medium& m = a_media[std::size_t(pr.domain)];
        double mun = pr.mun, mup = pr.mup;
        ed.muN = std::max(ed.muN, pr.mun);
        ed.muP = std::max(ed.muP, pr.mup);
        // (A magnetic field: the current along the field's own, J0/(1 +
        // β²); its turned part in hallFluxes.)
        if (hall()) {
            const double B2 = a_B[0] * a_B[0] + a_B[1] * a_B[1] + a_B[2] * a_B[2];
            mun /= 1 + m.rHn * m.rHn * pr.mun * pr.mun * B2;
            mup /= 1 + m.rHp * m.rHp * pr.mup * pr.mup * B2;
        }
        ed.wn += part.c * mun;
        ed.wp += part.c * mup;
    }
    // An edge into a heterojunction's other side: the step's emission, G =
    // v_R (1 + δ) over the edge's face across the interface, in series at its
    // end there with its Scharfetter and Gummel flux - the carriers crossing
    // it no faster than they arrive at it thermally (Crowell and Sze's), δ
    // what tunnels through it: the flux times G/(G + w Vt B), B its
    // Bernoulli toward that end, of x as solved (held a Newton step; 1
    // without it) - 2D's.
    if (a_emission > 0) {
        std::vector<double> field, count;
        std::vector<double> lgN, lgP;
        if (x) {
            degeneracy(*x, lgN, lgP);
            if (a_emission == 2) {
                field.assign(a_edges.size(), 0.0);
                count.assign(a_edges.size(), 0.0);
                for (int e = 0; e < int(a_prisms.size()); ++e) {
                    const Prism& pm = a_prisms[std::size_t(e)];
                    if (pm.domain < 0) continue;
                    double E[3];
                    fieldIn(e, *x, E);
                    const double v = std::sqrt(E[0] * E[0] + E[1] * E[1] + E[2] * E[2]);
                    for (int k = 0; k < pm.edgeCount; ++k) {
                        field[std::size_t(pm.edges[std::size_t(k)])] += v;
                        count[std::size_t(pm.edges[std::size_t(k)])] += 1;
                    }
                }
            }
        }
        for (std::size_t k = 0; k < a_edges.size(); ++k) {
            Edge& ed = a_edges[k];
            if (!(ed.across > 0) || ed.wn <= 0) continue;
            const double face = ed.geo * ed.length * ed.across;
            const double F = !field.empty() && count[k] > 0 ? field[k] / count[k] : 0.0;
            double Dn = 0, Dp = 0;
            if (x) {
                const EdgeState st = edgeState(ed, *x, lgN, lgP);
                Dn = st.Dn;
                Dp = st.Dp;
            }
            const double Gn = ed.vRn * (1 + (field.empty() ? 0.0 : detail::tunnelling(F, ed.stepN, ed.massN, KB * a_T))) * face;
            const double Gp = ed.vRp * (1 + (field.empty() ? 0.0 : detail::tunnelling(F, ed.stepP, ed.massP, KB * a_T))) * face;
            ed.wn *= Gn / (Gn + ed.wn * a_Vt * bern(ed.emitsAtI ? -Dn : Dn));
            ed.wp *= Gp / (Gp + ed.wp * a_Vt * bern(ed.emitsAtI ? Dp : -Dp));
        }
    }
}

void SemiconductorProblem3::carrierTemperatures(const Eigen::VectorXd& x)
{
    // Each carrier's energy balance by its nodes' boxes, its fluxes the
    // edges' as weighed now (their mobilities a Newton step behind):
    //   Σ S + (3/2) k n V (T - TL)/τw = what the field gives it,
    // S the energy each edge carries - the particles' (5/2) kT each and the
    // carriers' conduction of heat, κ = (5/2)(k²/q) µ n T (Wiedemann and
    // Franz's), Scharfetter and Gummel's fitting for both, D (B(-P) Ti -
    // B(P) Tj), P the particles' (5/2) k F over D; the field's gift each
    // prism's: its particles' flux, fitted to its edges' (each over its
    // face), times its band's slope, over its volume - a sixth to each
    // corner. Contacts and media without it at the lattice's temperature.
    // (Linear in T: κ's T and n the last's; the 2D solver's.)
    const std::size_t Nn = std::size_t(a_n);
    const double Vt = a_Vt, C0 = a_C0;
    std::vector<double> lgN, lgP;
    degeneracy(x, lgN, lgP);
    for (int carrier = 0; carrier < 2; ++carrier) {
        const bool electrons = carrier == 0;
        std::vector<double>& T = electrons ? a_Tn : a_Tp;
        if (T.size() != Nn) T = a_TL;
        std::vector<int> row(Nn, -1);
        int rows = 0;
        for (int i = 0; i < a_n; ++i) {
            const int d = a_nodeMedium[std::size_t(i)];
            if (d >= 0 && a_heldKind[std::size_t(i)] < 0 && a_media[std::size_t(d)].energy) row[std::size_t(i)] = rows++;
        }
        if (rows == 0) {
            T = a_TL;
            continue;
        }
        std::vector<Eigen::Triplet<double>> trip;
        Eigen::VectorXd b = Eigen::VectorXd::Zero(rows);
        std::vector<double> flows(a_edges.size(), 0.0), corners(Nn, 0.0);
        auto put = [&row, &trip, &b, &T](int r, int j, double c) {
            if (r < 0) return;
            if (row[std::size_t(j)] >= 0) trip.emplace_back(r, row[std::size_t(j)], c);
            else b(r) -= c * T[std::size_t(j)];
        };
        for (std::size_t k = 0; k < a_edges.size(); ++k) {
            const Edge& e = a_edges[k];
            const double w = electrons ? e.wn : e.wp;
            if (w <= 0 || a_nodeMedium[std::size_t(e.i)] < 0 || a_nodeMedium[std::size_t(e.j)] < 0) continue;
            const int i = e.i, j = e.j, ri = row[std::size_t(i)], rj = row[std::size_t(j)];
            double flow = 0, ci = 0, cj = 0;   // particles i to j (1/s); the densities
            const EdgeState st = edgeState(e, x, lgN, lgP);
            if (electrons) {
                ci = st.ni * C0;
                cj = st.nj * C0;
                flow = w * Vt * (ci * bern(-st.Dn) - cj * bern(st.Dn));
            } else {
                ci = st.pi * C0;
                cj = st.pj * C0;
                flow = w * Vt * (ci * bern(st.Dp) - cj * bern(-st.Dp));
            }
            flows[k] = flow;
            if (ri < 0 && rj < 0) continue;
            const double c = std::sqrt(std::max(ci, 1e-300) * std::max(cj, 1e-300));
            const double Te = 0.5 * (T[std::size_t(i)] + T[std::size_t(j)]);
            const double Dk = std::max(2.5 * KB * KB / Q * w * c * Te, 1e-300);
            const double P = 2.5 * KB * flow / Dk;
            const double bi = Dk * bern(-P), bj = Dk * bern(P);
            put(ri, i, bi);
            put(ri, j, -bj);
            put(rj, i, -bi);
            put(rj, j, bj);
        }
        const Mesh3& mesh = a_s.mesh();
        for (int e = 0; e < int(a_prisms.size()); ++e) {
            const Prism& pr = a_prisms[std::size_t(e)];
            if (pr.domain < 0 || !(pr.volume > 0)) continue;
            const auto& cn = mesh.prisms[std::size_t(e)];
            for (int k = 0; k < 6; ++k) corners[std::size_t(cn[std::size_t(k)])] += pr.volume / 6;
            // (Its particles' flux density: least squares over its edges,
            // each weighed by its face, its flow along its direction.)
            double M[3][3] = {}, rhs[3] = {0, 0, 0};
            for (int k = 0; k < pr.edgeCount; ++k) {
                const Edge& ed = a_edges[std::size_t(pr.edges[std::size_t(k)])];
                const double face = ed.geo * ed.length;
                if (!(face > 0) || (electrons ? ed.wn : ed.wp) <= 0) continue;
                const double fl = flows[std::size_t(pr.edges[std::size_t(k)])];
                const double t[3] = {ed.t.x, ed.t.y, ed.t.z};
                for (int a = 0; a < 3; ++a) {
                    rhs[a] += t[a] * fl;
                    for (int c = 0; c < 3; ++c) M[a][c] += face * t[a] * t[c];
                }
            }
            Eigen::Matrix3d Mm;
            for (int a = 0; a < 3; ++a)
                for (int c = 0; c < 3; ++c) Mm(a, c) = M[a][c];
            const double tr = Mm.trace();
            if (!(std::abs(Mm.determinant()) > 1e-9 * tr * tr * tr)) continue;
            const Eigen::Vector3d f = Mm.inverse() * Eigen::Vector3d(rhs[0], rhs[1], rhs[2]);
            // (Its band's edge, -q (V + Vt an) - an electron's gift the drop
            // along its flux; a hole's, -q (V - Vt ap), the rise.)
            double g[3] = {0, 0, 0};
            for (int k = 0; k < 6; ++k) {
                const int c = cn[std::size_t(k)];
                const double u = x(iV(c)) + (electrons ? Vt * a_an[std::size_t(c)] : -Vt * a_ap[std::size_t(c)]);
                for (int a = 0; a < 3; ++a) g[a] += pr.grad[k][a] * u;
            }
            const double gift = (electrons ? Q : -Q) * (f(0) * g[0] + f(1) * g[1] + f(2) * g[2]) * pr.volume;
            for (int k = 0; k < 6; ++k) {
                const int r = row[std::size_t(cn[std::size_t(k)])];
                if (r >= 0) b(r) += gift / 6;
            }
        }
        for (int i = 0; i < a_n; ++i) {
            const int r = row[std::size_t(i)];
            if (r < 0) continue;
            const Medium& m = a_media[std::size_t(a_nodeMedium[std::size_t(i)])];
            const double c = (electrons ? x(iN(i)) : x(iP(i))) * C0;
            // (Its relaxation; a node all but empty held by a millionth of
            // its doping's, near the lattice.)
            const double floor = 1e-6 * std::max(std::abs(a_Nd[std::size_t(i)] - a_Na[std::size_t(i)]), m.ni);
            const double k = 1.5 * KB * std::max(c, floor) * corners[std::size_t(i)] / (electrons ? m.tauWn : m.tauWp);
            trip.emplace_back(r, r, k);
            b(r) += k * a_TL[std::size_t(i)];
        }
        Eigen::SparseMatrix<double> A(rows, rows);
        A.setFromTriplets(trip.begin(), trip.end());
        Eigen::VectorXd scale = Eigen::VectorXd::Zero(rows);
        for (int k = 0; k < A.outerSize(); ++k)
            for (Eigen::SparseMatrix<double>::InnerIterator it(A, k); it; ++it) scale(it.row()) = std::max(scale(it.row()), std::abs(it.value()));
        for (int k = 0; k < A.outerSize(); ++k)
            for (Eigen::SparseMatrix<double>::InnerIterator it(A, k); it; ++it) it.valueRef() /= scale(it.row()) > 0 ? scale(it.row()) : 1.0;
        for (int r = 0; r < rows; ++r) b(r) /= scale(r) > 0 ? scale(r) : 1.0;
        SparseLu<double> lu;
        lu.compute(A);
        if (lu.info() != Eigen::Success) continue;   // (the last kept)
        const Eigen::VectorXd t = lu.solve(b);
        if (!t.allFinite()) continue;
        // (Each node's step, of its update r = solved - T, its secant's: T
        // - r/r' - ω r, ω = 1/(1 - slope) from a half to one; all of them
        // at most ω of the way, halved where the updates grew: 2D's.)
        std::vector<double>& was = electrons ? a_TnWas : a_TpWas;
        std::vector<double>& rWas = electrons ? a_dTnWas : a_dTpWas;
        const bool secant = was.size() == Nn && rWas.size() == Nn;
        std::vector<double> now = T, rNow(Nn, 0.0);
        double residual = 0;
        for (int i = 0; i < a_n; ++i) {
            const std::size_t k = std::size_t(i);
            const double solved = row[k] >= 0 ? std::max(t(row[k]), 0.5 * a_TL[k]) : a_TL[k];
            rNow[k] = solved - T[k];
            residual = std::max(residual, std::abs(rNow[k]) / a_TL[k]);
        }
        double& omegaAll = a_omegaT[carrier];
        if (!secant) omegaAll = 1;
        else if (residual > a_residualT[carrier]) omegaAll = std::max(0.02, 0.5 * omegaAll);
        else omegaAll = std::min(1.0, 1.5 * omegaAll);
        a_residualT[carrier] = residual;
        for (int i = 0; i < a_n; ++i) {
            const std::size_t k = std::size_t(i);
            const double r = rNow[k];
            double omega = 0.5;
            if (secant && std::abs(T[k] - was[k]) > 1e-9 * T[k]) {
                const double slope = (r - rWas[k]) / (T[k] - was[k]);
                if (slope < 0) omega = std::clamp(-1 / slope, 0.5, 1.0);
            }
            T[k] += std::min(omega, omegaAll) * r;
        }
        was = std::move(now);
        rWas = std::move(rNow);
    }
}

void SemiconductorProblem3::degeneracy(const Eigen::VectorXd& x, std::vector<double>& lgN, std::vector<double>& lgP, std::vector<double>* dlgN,
                                       std::vector<double>* dlgP) const
{
    lgN.assign(std::size_t(a_n), 0.0);
    lgP.assign(std::size_t(a_n), 0.0);
    if (dlgN) dlgN->assign(std::size_t(a_n), 0.0);
    if (dlgP) dlgP->assign(std::size_t(a_n), 0.0);
    for (int i = 0; i < a_n; ++i) {
        const int d = a_nodeMedium[std::size_t(i)];
        if (d < 0 || !a_media[std::size_t(d)].fermi) continue;
        const Medium& m = a_media[std::size_t(d)];
        const auto& fh = detail::fermiHalf();
        for (int c = 0; c < 2; ++c) {
            const double dens = std::max(x(c == 0 ? iN(i) : iP(i)) * a_C0, 1e-300), Nb = c == 0 ? m.Nc : m.Nv;
            const double eta = fh.inverse(dens / Nb);
            double Fv = 0, dF = 0;
            fh.at(eta, &Fv, &dF);
            (c == 0 ? lgN : lgP)[std::size_t(i)] = std::log(Fv) - eta;
            // d ln γ / d(carrier, scaled) = (F'/F - 1)/(Nb F') C0.
            std::vector<double>* dl = c == 0 ? dlgN : dlgP;
            if (dl) (*dl)[std::size_t(i)] = (dF / Fv - 1) / (Nb * dF) * a_C0;
        }
    }
}

void SemiconductorProblem3::impactIonization(const Eigen::VectorXd& x, const std::vector<double>& lgN, const std::vector<double>& lgP, Eigen::VectorXd& F,
                                             const std::function<void(int, int, double)>& add) const
{
    bool any = false;
    for (const Medium& m : a_media) any = any || (m.semiconductor && m.ionization && m.ionCurrents);
    a_ionRate.assign(any ? a_prisms.size() : 0, 0.0);
    if (!any) return;
    const double Vt = a_Vt;
    const Mesh3& mesh = a_s.mesh();
    // Each edge's particle flux densities along it (over C0; electrons'
    // sign turned: a current's direction), their rounding (10⁻¹⁰ of the two
    // terms they are the difference of) and their changes by (n or p at i,
    // at j, V at i, at j). Its mobility its prisms' mean, bounded by their
    // largest.
    struct EdgeCurrent {
        double jn = 0, jp = 0, fn = 0, fp = 0;
        double dn[4] = {0, 0, 0, 0}, dp[4] = {0, 0, 0, 0};
        bool on = false;
    };
    std::vector<EdgeCurrent> ec(a_edges.size());
    for (std::size_t k = 0; k < a_edges.size(); ++k) {
        const Edge& e = a_edges[k];
        if (e.geo <= 0 || e.wn <= 0 || e.length <= 0 || a_nodeMedium[std::size_t(e.i)] < 0 || a_nodeMedium[std::size_t(e.j)] < 0) continue;
        const int i = e.i, j = e.j;
        const double gn = std::min(e.wn / e.geo, e.muN) * Vt / e.length, gp = std::min(e.wp / e.geo, e.muP) * Vt / e.length;
        const EdgeState st = edgeState(e, x, lgN, lgP);
        const double Dn = st.Dn, Dp = st.Dp;
        const double bnp = bern(Dn), bnm = bern(-Dn), bpp = bern(Dp), bpm = bern(-Dp);
        EdgeCurrent& c = ec[k];
        c.on = true;
        // (Electrons: their current against their flux, gn (nj B(D) - ni B(-D)).)
        c.jn = gn * (st.nj * bnp - st.ni * bnm);
        c.jp = gp * (st.pi * bpp - st.pj * bpm);
        c.fn = 1e-10 * gn * (std::abs(st.nj * bnp) + std::abs(st.ni * bnm));
        c.fp = 1e-10 * gp * (std::abs(st.pi * bpp) + std::abs(st.pj * bpm));
        const double dnV = gn * (st.nj * dbern(Dn) + st.ni * dbern(-Dn)) / Vt;
        const double dpV = gp * (st.pi * dbern(Dp) + st.pj * dbern(-Dp)) / Vt;
        c.dn[0] = -gn * bnm * e.fNi;
        c.dn[1] = gn * bnp * e.fNj;
        c.dn[2] = -dnV;
        c.dn[3] = dnV;
        c.dp[0] = gp * bpp * e.fPi;
        c.dp[1] = -gp * bpm * e.fPj;
        c.dp[2] = -dpV;
        c.dp[3] = dpV;
    }
    // (Each node's quasi-Fermi potentials, where they drive it.)
    const auto phiN = [&](int k) { return x(iV(k)) - Vt * (std::log(std::max(x(iN(k)) * a_C0, 1e-300)) - lgN[std::size_t(k)] - a_an[std::size_t(k)]); };
    const auto phiP = [&](int k) { return x(iV(k)) + Vt * (std::log(std::max(x(iP(k)) * a_C0, 1e-300)) - lgP[std::size_t(k)] - a_ap[std::size_t(k)]); };
    for (int e = 0; e < int(a_prisms.size()); ++e) {
        const Prism& pr = a_prisms[std::size_t(e)];
        if (pr.domain < 0 || pr.edgeCount < 3) continue;
        const Medium& m = a_media[std::size_t(pr.domain)];
        if (!m.ionization || !m.ionCurrents) continue;
        const auto& cn = mesh.prisms[std::size_t(e)];
        bool corners = true;
        for (int k = 0; k < 6; ++k) corners = corners && a_nodeMedium[std::size_t(cn[std::size_t(k)])] >= 0;
        if (!corners) continue;
        double E[3];
        fieldIn(e, x, E);
        const double Ef = std::sqrt(E[0] * E[0] + E[1] * E[1] + E[2] * E[2]);
        double En = Ef, Ep = Ef, Fn[3] = {0, 0, 0}, Fp[3] = {0, 0, 0};
        if (m.ionFermi) {
            for (int k = 0; k < 6; ++k) {
                const double a = phiN(cn[std::size_t(k)]), b = phiP(cn[std::size_t(k)]);
                for (int c = 0; c < 3; ++c) {
                    Fn[c] += pr.grad[k][c] * a;
                    Fp[c] += pr.grad[k][c] * b;
                }
            }
            En = std::sqrt(Fn[0] * Fn[0] + Fn[1] * Fn[1] + Fn[2] * Fn[2]);
            Ep = std::sqrt(Fp[0] * Fp[0] + Fp[1] * Fp[1] + Fp[2] * Fp[2]);
        }
        double TL = 0;
        for (int k = 0; k < 6; ++k) TL += a_TL[std::size_t(cn[std::size_t(k)])] / 6;
        const double gT = detail::phononGamma(m.ionPhonon, TL);
        double an = 0, dan = 0, ap = 0, dap = 0;
        if (!m.ionAlong) {
            detail::ionizationRate(En, gT * m.an, gT * m.bn, gT * m.an2, gT * m.bn2, m.E0, m.ionBetaN, &an, &dan);
            detail::ionizationRate(Ep, gT * m.ap, gT * m.bp, gT * m.ap2, gT * m.bp2, m.E0, m.ionBetaP, &ap, &dap);
            if (an == 0 && ap == 0) continue;
        }
        // Its currents, J = (Σ t tᵀ)⁻¹ Σ t j over its edges - by the field,
        // each edge weighed by how far it stands above its rounding (2D's).
        const int K = pr.edgeCount;
        Eigen::Matrix3d M = Eigen::Matrix3d::Zero();
        for (int k = 0; k < K; ++k) {
            const Vec3& t = a_edges[std::size_t(pr.edges[std::size_t(k)])].t;
            const Eigen::Vector3d tv(t.x, t.y, t.z);
            M += tv * tv.transpose();
        }
        const double det = M.determinant();
        if (!(std::abs(det) > 1e-12)) continue;
        const Eigen::Matrix3d Mi = M.inverse();
        std::vector<Eigen::Vector3d> cnv(static_cast<std::size_t>(K)), cpv(static_cast<std::size_t>(K));
        for (int k = 0; k < K; ++k) {
            const Vec3& t = a_edges[std::size_t(pr.edges[std::size_t(k)])].t;
            cnv[std::size_t(k)] = cpv[std::size_t(k)] = Mi * Eigen::Vector3d(t.x, t.y, t.z);
        }
        const bool byField = !m.ionAlong && !m.ionFermi;
        double J0Fn = 0, J0Fp = 0;
        if (byField) {
            const auto weighed = [&](bool holes, std::vector<Eigen::Vector3d>& cv) {
                double rho = 0, top = 0;
                std::vector<double> w(std::size_t(K), 1.0);
                for (int k = 0; k < K; ++k) {
                    const EdgeCurrent& c = ec[std::size_t(pr.edges[std::size_t(k)])];
                    rho = std::max(rho, std::abs(holes ? c.jp : c.jn));
                }
                for (int k = 0; k < K; ++k) {
                    const EdgeCurrent& c = ec[std::size_t(pr.edges[std::size_t(k)])];
                    const double f = holes ? c.fp : c.fn;
                    w[std::size_t(k)] = rho > 0 ? 1 / (1 + (f / rho) * (f / rho)) : 1.0;
                    top = std::max(top, w[std::size_t(k)]);
                }
                Eigen::Matrix3d A = Eigen::Matrix3d::Zero();
                for (int k = 0; k < K; ++k) {
                    const Vec3& t = a_edges[std::size_t(pr.edges[std::size_t(k)])].t;
                    w[std::size_t(k)] /= top;
                    const Eigen::Vector3d tv(t.x, t.y, t.z);
                    A += w[std::size_t(k)] * tv * tv.transpose();
                }
                if (!(std::abs(A.determinant()) > 1e-3 * std::abs(det))) return;
                const Eigen::Matrix3d Ai = A.inverse();
                for (int k = 0; k < K; ++k) {
                    const Vec3& t = a_edges[std::size_t(pr.edges[std::size_t(k)])].t;
                    cv[std::size_t(k)] = w[std::size_t(k)] * (Ai * Eigen::Vector3d(t.x, t.y, t.z));
                }
            };
            weighed(false, cnv);
            weighed(true, cpv);
            for (int k = 0; k < K; ++k) {
                const EdgeCurrent& c = ec[std::size_t(pr.edges[std::size_t(k)])];
                J0Fn += cnv[std::size_t(k)].norm() * c.fn;
                J0Fp += cpv[std::size_t(k)].norm() * c.fp;
            }
        }
        Eigen::Vector3d Jnv = Eigen::Vector3d::Zero(), Jpv = Eigen::Vector3d::Zero();
        for (int k = 0; k < K; ++k) {
            const EdgeCurrent& c = ec[std::size_t(pr.edges[std::size_t(k)])];
            Jnv += cnv[std::size_t(k)] * c.jn;
            Jpv += cpv[std::size_t(k)] * c.jp;
        }
        const double Jn = Jnv.norm(), Jp = Jpv.norm();
        const Eigen::Vector3d Ev(E[0], E[1], E[2]);
        // (Along each current, E·J/s, s = √(|J|² + J0²), J0 a millionth of
        // the carrier's drift at the field: nought smoothly where drift and
        // diffusion balance.)
        Eigen::Vector3d un = Eigen::Vector3d::Zero(), up = Eigen::Vector3d::Zero();
        double sN = 0, sP = 0, J0n = 0, J0p = 0, cN = 0, cP = 0, nbar = 0, pbar = 0;
        if (m.ionAlong) {
            for (int k = 0; k < 6; ++k) {
                nbar += x(iN(cn[std::size_t(k)])) / 6;
                pbar += x(iP(cn[std::size_t(k)])) / 6;
            }
            cN = 1e-6 * pr.mun;
            cP = 1e-6 * pr.mup;
            J0n = cN * nbar * Ef;
            J0p = cP * pbar * Ef;
            sN = std::hypot(Jn, J0n);
            sP = std::hypot(Jp, J0p);
            if (sN > 0) un = Jnv / sN;
            if (sP > 0) up = Jpv / sP;
            En = std::max(Ev.dot(un), 0.0);
            Ep = std::max(Ev.dot(up), 0.0);
            detail::ionizationRate(En, gT * m.an, gT * m.bn, gT * m.an2, gT * m.bn2, m.E0, m.ionBetaN, &an, &dan);
            detail::ionizationRate(Ep, gT * m.ap, gT * m.bp, gT * m.ap2, gT * m.bp2, m.E0, m.ionBetaP, &ap, &dap);
            if (an == 0 && ap == 0) continue;
            if (!(En > 0)) dan = 0;
            if (!(Ep > 0)) dap = 0;
        }
        double JnI = Jn, JpI = Jp, sFn = Jn, sFp = Jp;
        if (byField) {
            sFn = std::hypot(Jn, J0Fn);
            sFp = std::hypot(Jp, J0Fp);
            JnI = sFn > 0 ? Jn * Jn / sFn : 0.0;
            JpI = sFp > 0 ? Jp * Jp / sFp : 0.0;
        }
        const double share = pr.volume / 6;
        const double g = (an * JnI + ap * JpI) * share;   // pairs/s over C0, to each corner
        a_ionPairs += 6 * g * a_C0;
        a_ionRate[std::size_t(e)] = 6 * g * a_C0 / pr.volume;
        for (int k = 0; k < 6; ++k) {
            F(iN(cn[std::size_t(k)])) -= g;
            F(iP(cn[std::size_t(k)])) -= g;
        }
        // Its change: by the field (each corner's V), by the currents (each
        // edge's ends' n, p, V).
        std::map<int, double> dg;
        if (m.ionFermi) {
            for (int k = 0; k < 6; ++k) {
                const int i = cn[std::size_t(k)];
                if (En > 0) {
                    const double d = (Fn[0] * pr.grad[k][0] + Fn[1] * pr.grad[k][1] + Fn[2] * pr.grad[k][2]) / En * dan * Jn * share;
                    dg[iV(i)] += d;
                    dg[iN(i)] += -d * Vt / std::max(x(iN(i)), 1e-300);
                }
                if (Ep > 0) {
                    const double d = (Fp[0] * pr.grad[k][0] + Fp[1] * pr.grad[k][1] + Fp[2] * pr.grad[k][2]) / Ep * dap * Jp * share;
                    dg[iV(i)] += d;
                    dg[iP(i)] += d * Vt / std::max(x(iP(i)), 1e-300);
                }
            }
        } else if (m.ionAlong) {
            for (int k = 0; k < 6; ++k) {
                const int i = cn[std::size_t(k)];
                const Eigen::Vector3d gk(pr.grad[k][0], pr.grad[k][1], pr.grad[k][2]);
                const double gE = Ef > 0 ? Ev.dot(gk) / Ef : 0.0;   // -d|E|/dV
                const double wn = sN > 0 ? En * J0n / (sN * sN) : 0.0, wp = sP > 0 ? Ep * J0p / (sP * sP) : 0.0;
                dg[iV(i)] += (dan * Jn * (-un.dot(gk) + wn * cN * nbar * gE) + dap * Jp * (-up.dot(gk) + wp * cP * pbar * gE)) * share;
                dg[iN(i)] += -dan * Jn * wn * cN * Ef / 6 * share;
                dg[iP(i)] += -dap * Jp * wp * cP * Ef / 6 * share;
            }
        } else if (Ef > 0) {
            for (int k = 0; k < 6; ++k) {
                const double dE = -(E[0] * pr.grad[k][0] + E[1] * pr.grad[k][1] + E[2] * pr.grad[k][2]) / Ef;
                dg[iV(cn[std::size_t(k)])] += (dan * JnI + dap * JpI) * dE * share;
            }
        }
        for (int k = 0; k < K; ++k) {
            const Edge& ed = a_edges[std::size_t(pr.edges[std::size_t(k)])];
            const EdgeCurrent& c = ec[std::size_t(pr.edges[std::size_t(k)])];
            if (!c.on) continue;
            const Eigen::Vector3d& vn = cnv[std::size_t(k)];
            const Eigen::Vector3d& vp = cpv[std::size_t(k)];
            double sn = Jn > 0 ? an * Jnv.dot(vn) / Jn : 0.0, sp = Jp > 0 ? ap * Jpv.dot(vp) / Jp : 0.0;
            if (byField) {
                sn = sFn > 0 ? an * Jnv.dot(vn) * (Jn * Jn + 2 * J0Fn * J0Fn) / (sFn * sFn * sFn) : 0.0;
                sp = sFp > 0 ? ap * Jpv.dot(vp) * (Jp * Jp + 2 * J0Fp * J0Fp) / (sFp * sFp * sFp) : 0.0;
            }
            if (m.ionAlong) {
                if (sN > 0) sn += dan * Jn / sN * (Ev - En * un).dot(vn);
                if (sP > 0) sp += dap * Jp / sP * (Ev - Ep * up).dot(vp);
            }
            const int cols[4][2] = {{iN(ed.i), iP(ed.i)}, {iN(ed.j), iP(ed.j)}, {iV(ed.i), iV(ed.i)}, {iV(ed.j), iV(ed.j)}};
            for (int q = 0; q < 4; ++q) {
                dg[cols[q][0]] += sn * c.dn[q] * share;
                dg[cols[q][1]] += sp * c.dp[q] * share;
            }
        }
        for (int k = 0; k < 6; ++k)
            for (const auto& [col, d] : dg) {
                if (d == 0) continue;
                add(iN(cn[std::size_t(k)]), col, -d);
                add(iP(cn[std::size_t(k)]), col, -d);
            }
    }
}

void SemiconductorProblem3::hallFluxes(const Eigen::VectorXd& x, const std::vector<double>& lgN, const std::vector<double>& lgP, const std::vector<double>& dlgN,
                                       const std::vector<double>& dlgP, Eigen::VectorXd& F, const std::function<void(int, int, double)>& add,
                                       std::vector<double>* edgeFlux) const
{
    if (edgeFlux) edgeFlux->assign(2 * a_edges.size(), 0.0);
    const double Vt = a_Vt;
    for (const Prism& pr : a_prisms) {
        if (pr.domain < 0 || pr.edgeCount < 3) continue;
        const Medium& m = a_media[std::size_t(pr.domain)];
        // (Not across a heterojunction's step: its carriers emitted there.)
        bool step = false;
        for (int k = 0; k < pr.edgeCount && !step; ++k) {
            const Edge& e = a_edges[std::size_t(pr.edges[std::size_t(k)])];
            for (int i : {e.i, e.j}) {
                const int d = a_nodeMedium[std::size_t(i)];
                step = step || d < 0 || (d != pr.domain && (a_media[std::size_t(d)].Eg != m.Eg || a_media[std::size_t(d)].chi != m.chi));
            }
        }
        if (step) continue;
        // Its flux density without the field, fitted to its edges' (each
        // Scharfetter and Gummel's at its mobility): Γ0 = (Σ t tᵀ)⁻¹ Σ t γ -
        // exact where it is uniform.
        double M[3][3] = {};
        for (int k = 0; k < pr.edgeCount; ++k) {
            const Vec3& t = a_edges[std::size_t(pr.edges[std::size_t(k)])].t;
            const double tv[3] = {t.x, t.y, t.z};
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) M[a][b] += tv[a] * tv[b];
        }
        const double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
                           + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
        if (!(std::abs(det) > 1e-12)) continue;
        double Mi[3][3];
        Mi[0][0] = (M[1][1] * M[2][2] - M[1][2] * M[2][1]) / det;
        Mi[0][1] = (M[0][2] * M[2][1] - M[0][1] * M[2][2]) / det;
        Mi[0][2] = (M[0][1] * M[1][2] - M[0][2] * M[1][1]) / det;
        Mi[1][0] = (M[1][2] * M[2][0] - M[1][0] * M[2][2]) / det;
        Mi[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) / det;
        Mi[1][2] = (M[0][2] * M[1][0] - M[0][0] * M[1][2]) / det;
        Mi[2][0] = (M[1][0] * M[2][1] - M[1][1] * M[2][0]) / det;
        Mi[2][1] = (M[0][1] * M[2][0] - M[0][0] * M[2][1]) / det;
        Mi[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) / det;
        for (int c = 0; c < 2; ++c) {
            const double mu = c == 0 ? pr.mun : pr.mup, r = c == 0 ? m.rHn : m.rHp;
            const double sgn = c == 0 ? -1.0 : 1.0;
            const double beta[3] = {sgn * r * mu * a_B[0], sgn * r * mu * a_B[1], sgn * r * mu * a_B[2]};
            const double b2 = beta[0] * beta[0] + beta[1] * beta[1] + beta[2] * beta[2];
            if (b2 == 0) continue;
            // T Γ0 = (Γ0×β + (Γ0·β) β)/(1 + β²): T = (-[β]× + β βᵀ)/(1 + β²).
            double T[3][3];
            const double bx[3][3] = {{0, -beta[2], beta[1]}, {beta[2], 0, -beta[0]}, {-beta[1], beta[0], 0}};
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) T[a][b] = (-bx[a][b] + beta[a] * beta[b]) / (1 + b2);
            // Each edge's flux density along it (over C0), and its change.
            double g[9] = {}, dg[9][4] = {};
            int col[9][4];
            for (int k = 0; k < pr.edgeCount; ++k) {
                const Edge& e = a_edges[std::size_t(pr.edges[std::size_t(k)])];
                const int i = e.i, j = e.j;
                const double w = mu * Vt / e.length;
                col[k][2] = iV(i);
                col[k][3] = iV(j);
                const EdgeState st = edgeState(e, x, lgN, lgP);
                if (c == 0) {
                    const double D = st.Dn;
                    const double bp = bern(D), bm = bern(-D), dgdD = w * (-st.ni * dbern(-D) - st.nj * dbern(D));
                    g[k] = w * (st.ni * bm - st.nj * bp);
                    col[k][0] = iN(i);
                    col[k][1] = iN(j);
                    dg[k][0] = w * e.fNi * bm - dgdD * dlgN[std::size_t(i)];
                    dg[k][1] = -w * e.fNj * bp + dgdD * dlgN[std::size_t(j)];
                    dg[k][2] = -dgdD / Vt;
                    dg[k][3] = dgdD / Vt;
                } else {
                    const double D = st.Dp;
                    const double bp = bern(D), bm = bern(-D), dgdD = w * (st.pi * dbern(D) + st.pj * dbern(-D));
                    g[k] = w * (st.pi * bp - st.pj * bm);
                    col[k][0] = iP(i);
                    col[k][1] = iP(j);
                    dg[k][0] = w * e.fPi * bp + dgdD * dlgP[std::size_t(i)];
                    dg[k][1] = -w * e.fPj * bm - dgdD * dlgP[std::size_t(j)];
                    dg[k][2] = -dgdD / Vt;
                    dg[k][3] = dgdD / Vt;
                }
            }
            // Γ0 = Σ W_q g_q, W_q = M⁻¹ t_q; its turned part through each
            // edge's share of its dual face: (T Γ0)·t L c.
            double W[9][3];
            for (int q = 0; q < pr.edgeCount; ++q) {
                const Vec3& t = a_edges[std::size_t(pr.edges[std::size_t(q)])].t;
                for (int a = 0; a < 3; ++a) W[q][a] = Mi[a][0] * t.x + Mi[a][1] * t.y + Mi[a][2] * t.z;
            }
            for (int k = 0; k < pr.edgeCount; ++k) {
                const Edge& e = a_edges[std::size_t(pr.edges[std::size_t(k)])];
                const double face = pr.coupling[std::size_t(k)] * e.length;
                double Tt[3];   // Tᵀ t face: the flux per Γ0
                for (int a = 0; a < 3; ++a) Tt[a] = (T[0][a] * e.t.x + T[1][a] * e.t.y + T[2][a] * e.t.z) * face;
                const int ri = c == 0 ? iN(e.i) : iP(e.i), rj = c == 0 ? iN(e.j) : iP(e.j);
                double phi = 0;
                for (int q = 0; q < pr.edgeCount; ++q) {
                    const double coef = Tt[0] * W[q][0] + Tt[1] * W[q][1] + Tt[2] * W[q][2];
                    if (coef == 0) continue;
                    phi += coef * g[q];
                    for (int u = 0; u < 4; ++u) {
                        if (dg[q][u] == 0) continue;
                        add(ri, col[q][u], coef * dg[q][u]);
                        add(rj, col[q][u], -coef * dg[q][u]);
                    }
                }
                F(ri) += phi;
                F(rj) -= phi;
                if (edgeFlux) (*edgeFlux)[2 * std::size_t(pr.edges[std::size_t(k)]) + std::size_t(c)] += phi;
            }
        }
    }
}

// --------------------------------------------------------------- Assembly

double SemiconductorProblem3::recombination(int i, double n, double p, double field, double* dn, double* dp) const
{
    *dn = *dp = 0;
    const int d = a_nodeMedium[std::size_t(i)];
    if (d < 0) return 0;
    const Medium& m = a_media[std::size_t(d)];
    const double ni2 = std::exp(a_an[std::size_t(i)] + a_ap[std::size_t(i)]), ni = std::sqrt(ni2), ex = n * p - ni2;
    double R = 0;
    if (m.srh) {
        const double kT = KB * a_T;
        const double n1 = ni * std::exp(m.Et / kT), p1 = ni * std::exp(-m.Et / kT);
        double tn = a_tauN[std::size_t(i)], tp = a_tauP[std::size_t(i)];
        // Hurkx's trap-assisted tunnelling: each lifetime over 1 + Γ of the
        // field, the trap's depth below its band.
        if (m.tat && field > 0) {
            double gn = 0, gp = 0, dg = 0;
            // (The trap Et above the intrinsic level: Ec - Et, Et - Ev at the
            // node, its gap narrowed - 2D's.)
            const double gap = a_gap[std::size_t(i)], ecEi = m.dEc - gap / 2, eiEv = m.Eg - gap - ecEi;
            const double depthN = (ecEi - m.Et) / kT, depthP = (eiEv + m.Et) / kT;
            detail::hurkx(field, m.FGn, depthN, &gn, &dg);
            detail::hurkx(field, m.FGp, depthP, &gp, &dg);
            tn /= 1 + gn;
            tp /= 1 + gp;
        }
        const double den = tp * (n + n1) + tn * (p + p1);
        R += ex / den;
        *dn += (p * den - ex * tp) / (den * den);
        *dp += (n * den - ex * tn) / (den * den);
    }
    if (m.Cn != 0 || m.Cp != 0) {
        const double c = m.Cn * n + m.Cp * p;
        R += c * ex;
        *dn += m.Cn * ex + c * p;
        *dp += m.Cp * ex + c * n;
    }
    if (m.B != 0) {
        R += m.B * ex;
        *dn += m.B * p;
        *dp += m.B * n;
    }
    return R;
}

void SemiconductorProblem3::assemble(const Eigen::VectorXd& x, Eigen::VectorXd& F, std::vector<Eigen::Triplet<double>>* J, bool carriers) const
{
    F = Eigen::VectorXd::Zero(3 * a_n);
    if (J) J->clear();
    a_ionPairs = a_genPairs = 0;
    // Stimulated recombination's rate by node, of the last assembly's mean
    // recombination over its domains (its own derivative left out).
    if (!a_stimulated.empty() && carriers) {
        if (a_lastR.size() != std::size_t(a_n)) a_lastR.assign(std::size_t(a_n), 0.0);
        a_stimRate.assign(std::size_t(a_n), 0.0);
        for (const Stimulated& st : a_stimulated) {
            double volume = 0, recombined = 0;
            for (int i = 0; i < a_n; ++i) {
                const int d = a_nodeMedium[std::size_t(i)];
                if (d < 0 || !st.on[std::size_t(d)]) continue;
                volume += a_box[std::size_t(i)];
                recombined += a_box[std::size_t(i)] * a_lastR[std::size_t(i)];
            }
            const double rate = volume > 0 ? st.c * std::max(0.0, recombined / volume - st.R0) : 0.0;
            for (int i = 0; i < a_n; ++i) {
                const int d = a_nodeMedium[std::size_t(i)];
                if (d >= 0 && st.on[std::size_t(d)]) a_stimRate[std::size_t(i)] += rate;
            }
        }
    }
    const auto add = [J](int r, int c, double v) {
        if (J && v != 0) J->emplace_back(r, c, v);
    };
    const double C0 = a_C0, Vt = a_Vt;
    // Fermi-Dirac's degeneracy at each node: ln γ of its carriers, and its
    // change with them (γ = F½(η)/e^η).
    std::vector<double> lgN, lgP, dlgN, dlgP;
    if (carriers) degeneracy(x, lgN, lgP, &dlgN, &dlgP);
    else lgN = lgP = dlgN = dlgP = std::vector<double>(std::size_t(a_n), 0.0);
    // Poisson: Σ ε c (Vi - Vj) - q box (p - n + Nd⁺ - Na⁻) [C]; the carriers:
    // their particle flux out of each box, and recombination less
    // generation in it [1/s], each over C0.
    for (const Edge& e : a_edges) {
        const int i = e.i, j = e.j;
        const double dv = x(iV(i)) - x(iV(j));
        F(iV(i)) += e.eps * dv;
        F(iV(j)) -= e.eps * dv;
        add(iV(i), iV(i), e.eps);
        add(iV(i), iV(j), -e.eps);
        add(iV(j), iV(j), e.eps);
        add(iV(j), iV(i), -e.eps);
        if (!carriers || e.wn <= 0) continue;
        if (a_nodeMedium[std::size_t(i)] < 0 || a_nodeMedium[std::size_t(j)] < 0) continue;
        // Electrons i to j: µ Vt c (ni B(-Δ) - nj B(Δ)), Δ the step of their
        // effective potential V + Vt an (+ Vt ln γ) over Vt.
        double gnE = 0, gpE = 0;   // (the fluxes, for impact ionization)
        double dgn[6] = {0, 0, 0, 0, 0, 0}, dgp[6] = {0, 0, 0, 0, 0, 0};   // by Vi, Vj, ni, nj, pi, pj
        const EdgeState st = edgeState(e, x, lgN, lgP);
        {
            const double D = st.Dn;
            const double bp = bern(D), bm = bern(-D), dbp = dbern(D), dbm = dbern(-D);
            const double w = e.wn * Vt, ni = st.ni, nj = st.nj;
            const double g = w * (ni * bm - nj * bp);
            const double dgdD = w * (-ni * dbm - nj * dbp);
            dgn[0] = -dgdD / Vt;
            dgn[1] = dgdD / Vt;
            dgn[2] = w * e.fNi * bm - dgdD * dlgN[std::size_t(i)];
            dgn[3] = -w * e.fNj * bp + dgdD * dlgN[std::size_t(j)];
            gnE = g;
            F(iN(i)) += g;
            F(iN(j)) -= g;
            for (const auto& [row, sign] : {std::pair{iN(i), 1.0}, std::pair{iN(j), -1.0}}) {
                add(row, iV(i), sign * dgn[0]);
                add(row, iV(j), sign * dgn[1]);
                add(row, iN(i), sign * dgn[2]);
                add(row, iN(j), sign * dgn[3]);
            }
        }
        // Holes i to j: µ Vt c (pi B(Δ) - pj B(-Δ)), Δ of V - Vt ap (- Vt
        // ln γ).
        {
            const double D = st.Dp;
            const double bp = bern(D), bm = bern(-D), dbp = dbern(D), dbm = dbern(-D);
            const double w = e.wp * Vt, pi = st.pi, pj = st.pj;
            const double g = w * (pi * bp - pj * bm);
            const double dgdD = w * (pi * dbp + pj * dbm);
            dgp[0] = -dgdD / Vt;
            dgp[1] = dgdD / Vt;
            dgp[4] = w * e.fPi * bp + dgdD * dlgP[std::size_t(i)];
            dgp[5] = -w * e.fPj * bm - dgdD * dlgP[std::size_t(j)];
            gpE = g;
            F(iP(i)) += g;
            F(iP(j)) -= g;
            for (const auto& [row, sign] : {std::pair{iP(i), 1.0}, std::pair{iP(j), -1.0}}) {
                add(row, iV(i), sign * dgp[0]);
                add(row, iV(j), sign * dgp[1]);
                add(row, iP(i), sign * dgp[4]);
                add(row, iP(j), sign * dgp[5]);
            }
        }
    }
    // Impact ionization, by prism (2D's, by triangle): α of its driving
    // field times each carrier's current density, its currents fitted to
    // its edges' (exact where the current is uniform), a sixth of its
    // volume's pairs to each corner.
    if (carriers) impactIonization(x, lgN, lgP, F, add);
    // A magnetic field: each prism's currents turned, into its edges.
    if (carriers && hall()) hallFluxes(x, lgN, lgP, dlgN, dlgP, F, add);
    for (int i = 0; i < a_n; ++i) {
        if (!a_nodeOn[std::size_t(i)]) {
            // (Not in the physics: its unknowns their own.)
            for (int c = 0; c < 3; ++c) add(3 * i + c, 3 * i + c, 1);
            continue;
        }
        const int d = a_nodeMedium[std::size_t(i)];
        if (d < 0) {
            // An insulator's node: no carriers.
            add(iN(i), iN(i), 1);
            add(iP(i), iP(i), 1);
            F(iN(i)) = x(iN(i));
            F(iP(i)) = x(iP(i));
            continue;
        }
        const double box = a_box[std::size_t(i)];
        if (!carriers) {
            // At equilibrium: the carriers of V, the Fermi level the
            // contacts' (-q a_fermi).
            double n, p, dn, dp, nd, dnd, na, dna;
            carriersAt(i, x(iV(i)) - a_fermi, &n, &p, &dn, &dp);
            ionized(i, n, p, &nd, &dnd, &na, &dna);
            // (Its carriers in the other sides' parts of its box on their
            // bands: a_boxN, a_boxP.)
            const double bN = a_boxN[std::size_t(i)], bP = a_boxP[std::size_t(i)];
            F(iV(i)) -= Q * (bP * p - bN * n + box * (nd - na));
            add(iV(i), iV(i), -Q * (bP * dp - bN * dn + box * (dnd * dn - dna * dp)));
            add(iN(i), iN(i), 1);
            add(iP(i), iP(i), 1);
            F(iN(i)) = 0;
            F(iP(i)) = 0;
            continue;
        }
        const double n = x(iN(i)) * C0, p = x(iP(i)) * C0;
        double nd, dnd, na, dna;
        ionized(i, n, p, &nd, &dnd, &na, &dna);
        const double bN = a_boxN[std::size_t(i)], bP = a_boxP[std::size_t(i)];
        F(iV(i)) -= Q * (bP * p - bN * n + box * (nd - na));
        add(iV(i), iP(i), -Q * C0 * (bP - box * dna));
        add(iV(i), iN(i), Q * C0 * (bN - box * dnd));
        double dRn = 0, dRp = 0;
        // (Its recombination over the parts of its box on its own bands, its
        // generation over all of it: 2D's.)
        const double rbox = a_recBox[std::size_t(i)];
        const double recombined = recombination(i, n, p, a_field[std::size_t(i)], &dRn, &dRp);
        double RV = rbox * recombined - box * a_generation[std::size_t(i)];
        a_genPairs += box * a_generation[std::size_t(i)] + rbox * std::max(-recombined, 0.0);
        if (!a_stimulated.empty()) {
            a_lastR[std::size_t(i)] = recombined;
            if (a_stimRate.size() == std::size_t(a_n)) RV += box * a_stimRate[std::size_t(i)];
        }
        F(iN(i)) += RV / C0;
        F(iP(i)) += RV / C0;
        add(iN(i), iN(i), rbox * dRn);
        add(iN(i), iP(i), rbox * dRp);
        add(iP(i), iN(i), rbox * dRn);
        add(iP(i), iP(i), rbox * dRp);
        // A step in time: box ∂n/∂t, BDF's mass n - its history (the history
        // the stepper's, its own steps' already in it).
        if (a_mass != 0 && a_history) {
            const double hn = (*a_history)[std::size_t(Cn * a_n + i)], hp = (*a_history)[std::size_t(Cp * a_n + i)];
            F(iN(i)) += bN * (a_mass * n - hn) / C0;
            F(iP(i)) += bP * (a_mass * p - hp) / C0;
            add(iN(i), iN(i), bN * a_mass);
            add(iP(i), iP(i), bP * a_mass);
        }
    }
    // Band-to-band tunnelling, by prism: A E^γ e^(-B/E) at its field, the
    // pairs' sign and size its corners' (n p - ni²)/((n + ni)(p + ni)) -
    // Hurkx's - or generation alone, a sixth of its volume to each corner
    // (the field lagged in Newton's matrix).
    if (carriers) {
        const Mesh3& mesh = a_s.mesh();
        const Space3& sp = *a_s.fields()[std::size_t(a_fi)].space;
        for (int e = 0; e < int(a_prisms.size()); ++e) {
            const Prism& pr = a_prisms[std::size_t(e)];
            if (pr.domain < 0 || !a_media[std::size_t(pr.domain)].tunnelling) continue;
            const Medium& m = a_media[std::size_t(pr.domain)];
            // Its field at its middle (its linear prism's gradient).
            PrismShape s;
            prismShapeAt(1, 1.0 / 3, 1.0 / 3, 0.5, s);
            const Space3::Map mp = sp.map(e, 1.0 / 3, 1.0 / 3, 0.5);
            double gx = 0, gy = 0, gz = 0;
            const int* dofs = sp.dofs(e);
            for (int k = 0; k < 6; ++k) {
                double g[3];
                Space3::gradient(mp, s.dxi[k], s.deta[k], s.dzeta[k], g);
                gx += g[0] * x(iV(dofs[k]));
                gy += g[1] * x(iV(dofs[k]));
                gz += g[2] * x(iV(dofs[k]));
            }
            const double E = std::sqrt(gx * gx + gy * gy + gz * gz);
            if (!(E > 1e3)) continue;
            const double rate = m.btA * std::pow(E, m.btGamma) * std::exp(-m.btB / E);
            if (!(rate > 0)) continue;
            (void)mesh;
            for (int k = 0; k < 6; ++k) {
                const int i = dofs[k];
                if (a_nodeMedium[std::size_t(i)] < 0) continue;
                const double n = x(iN(i)) * C0, p = x(iP(i)) * C0, ni = std::exp(0.5 * (a_an[std::size_t(i)] + a_ap[std::size_t(i)]));
                const double den = (n + ni) * (p + ni), excess = n * p - ni * ni;
                const double Dv = m.btGeneration ? -1.0 : excess / den;
                const double dDn = m.btGeneration ? 0.0 : (p * den - excess * (p + ni)) / (den * den);
                const double dDp = m.btGeneration ? 0.0 : (n * den - excess * (n + ni)) / (den * den);
                const double share = pr.volume / 6;
                a_genPairs += rate * std::max(-Dv, 0.0) * share;
                F(iN(i)) += rate * Dv * share / C0;
                F(iP(i)) += rate * Dv * share / C0;
                add(iN(i), iN(i), rate * dDn * share);
                add(iN(i), iP(i), rate * dDp * share);
                add(iP(i), iN(i), rate * dDn * share);
                add(iP(i), iP(i), rate * dDp * share);
            }
        }
    }
    // Faces: surface charge; traps' charge, -q N f (acceptor-like) or q N
    // (1 - f), and the pairs they recombine (a surface's recombination a
    // level of no charge) - at Boltzmann's carriers (Fermi-Dirac's n/γ: the
    // traps filled by the Fermi level).
    for (const Surface& sf : a_surfaces)
        for (const auto& [i, area] : sf.nodes) {
            F(iV(i)) -= area * sf.rhos;
            const int d = a_nodeMedium[std::size_t(i)];
            if (sf.levels.empty() || d < 0) continue;
            const double ni = std::exp(0.5 * (a_an[std::size_t(i)] + a_ap[std::size_t(i)]));
            // The carriers the traps see, and their changes by unknown.
            double n, p, dnV = 0, dpV = 0, dnN = 0, dpP = 0;
            if (carriers) {
                const double gn = std::exp(-lgN[std::size_t(i)]), gp = std::exp(-lgP[std::size_t(i)]);
                n = x(iN(i)) * C0 * gn;
                p = x(iP(i)) * C0 * gp;
                dnN = gn * (C0 - x(iN(i)) * C0 * dlgN[std::size_t(i)]);
                dpP = gp * (C0 - x(iP(i)) * C0 * dlgP[std::size_t(i)]);
            } else {
                const double u = x(iV(i)) - a_fermi;
                n = std::exp(u / Vt + a_an[std::size_t(i)]);
                p = std::exp(-u / Vt + a_ap[std::size_t(i)]);
                dnV = n / Vt;
                dpV = -p / Vt;
            }
            for (const Surface::Level& lv : sf.levels) {
                const double n1 = ni * std::exp(lv.Et / (KB * a_T)), p1 = ni * std::exp(-lv.Et / (KB * a_T));
                const TrapState st = trapState(lv.N, sf.cn, sf.cp, n1, p1, n, p);
                if (sf.charged) {
                    F(iV(i)) += Q * lv.N * area * (sf.acceptor ? st.f : st.f - 1);
                    const double k = Q * lv.N * area;
                    if (carriers) {
                        add(iV(i), iN(i), k * st.dfdn * dnN);
                        add(iV(i), iP(i), k * st.dfdp * dpP);
                    } else
                        add(iV(i), iV(i), k * (st.dfdn * dnV + st.dfdp * dpV));
                }
                if (!carriers) continue;
                a_genPairs += area * std::max(-st.U, 0.0);
                F(iN(i)) += area * st.U / C0;
                F(iP(i)) += area * st.U / C0;
                add(iN(i), iN(i), area * st.dUdn * dnN / C0);
                add(iN(i), iP(i), area * st.dUdp * dpP / C0);
                add(iP(i), iN(i), area * st.dUdn * dnN / C0);
                add(iP(i), iP(i), area * st.dUdp * dpP / C0);
            }
        }
    // Schottky contacts: thermionic emission, vn (n - nB) out to the metal.
    if (carriers)
        for (const Contact& c : a_contacts) {
            if (c.kind != Contact::Schottky) continue;
            for (const auto& [i, area] : c.nodes) {
                const int d = a_nodeMedium[std::size_t(i)];
                if (d < 0) continue;
                const Medium& m = a_media[std::size_t(d)];
                const double nB = m.Nc * std::exp(-(c.phiB - a_gap[std::size_t(i)] / 2) / (KB * a_T));
                const double pB = std::exp(a_an[std::size_t(i)] + a_ap[std::size_t(i)]) / nB;
                F(iN(i)) += area * c.vn * (x(iN(i)) - nB / C0);
                F(iP(i)) += area * c.vp * (x(iP(i)) - pB / C0);
                add(iN(i), iN(i), area * c.vn);
                add(iP(i), iP(i), area * c.vp);
            }
        }
}

void SemiconductorProblem3::heldValues(const std::vector<double>& applied, std::vector<double>& held) const
{
    // By unknown: its held value, NaN where it is free.
    held.assign(std::size_t(3 * a_n), NaN);
    for (int c = 0; c < int(a_contacts.size()); ++c) {
        const Contact& ct = a_contacts[std::size_t(c)];
        const double v0 = applied[std::size_t(c)];
        for (const auto& [i, area] : ct.nodes) {
            (void)area;
            if (a_heldKind[std::size_t(i)] != c) continue;
            const int d = a_nodeMedium[std::size_t(i)];
            if (ct.kind == Contact::Gate) {
                // The metal's Fermi level at -q V0: the insulator's potential
                // there V0 - (φm - χ - dEc)/q of the reference semiconductor's
                // intrinsic level.
                held[std::size_t(iV(i))] = v0 - (ct.phim - a_refChi - a_refDEc) / Q;
                continue;
            }
            if (d < 0) continue;
            if (ct.kind == Contact::Ohmic) {
                // Its carriers neutral, its Fermi level the metal's.
                const double u = a_neutralU[std::size_t(i)];
                double n, p, dn, dp;
                carriersAt(i, u, &n, &p, &dn, &dp);
                held[std::size_t(iV(i))] = v0 + u;
                // (Its minority carriers free where a surface's recombination
                // alone takes them.)
                const bool freed = a_minorityFree[std::size_t(i)] != 0;
                if (!freed || n > p) held[std::size_t(iN(i))] = n / a_C0;
                if (!freed || p >= n) held[std::size_t(iP(i))] = p / a_C0;
            } else {
                // Its conduction band φB above the metal's Fermi level.
                const Medium& m = a_media[std::size_t(d)];
                const double nB = m.Nc * std::exp(-(ct.phiB - a_gap[std::size_t(i)] / 2) / (KB * a_T));
                double u = a_Vt * (std::log(nB) - a_an[std::size_t(i)]);
                if (m.fermi) u = a_Vt * (-(ct.phiB - a_gap[std::size_t(i)] / 2) / (KB * a_T) - a_an[std::size_t(i)] + std::log(m.Nc));
                held[std::size_t(iV(i))] = v0 + u;
            }
        }
    }
}

bool SemiconductorProblem3::newton(Eigen::VectorXd& x, const std::vector<double>& applied, bool carriers, int maxSteps, int* steps, QString* why)
{
    const int N = a_unknowns;
    if (x.size() != N) x.conservativeResize(N);
    if (!carriers)
        for (std::size_t c = 0; c < a_contacts.size(); ++c)
            if (a_contacts[c].iC >= 0) x(a_contacts[c].iC) = applied[c];
    std::vector<double> held;
    heldValues(heldAt(x, applied, carriers), held);
    for (int k = 0; k < 3 * a_n; ++k)
        if (!std::isnan(held[std::size_t(k)])) x(k) = held[std::size_t(k)];
    double lastRel = 0, lastT = 1;
    int worst = -1;
    bool polished = false;
    const bool lagged = carriers && (a_emission > 0 || std::any_of(a_media.begin(), a_media.end(), [](const Medium& m) { return m.semiconductor && m.varies(); }));
    bool tat = false;
    for (const Medium& m : a_media) tat = tat || (m.semiconductor && m.tat);
    for (int it = 0; it < maxSteps; ++it) {
        // (The mobilities and the field of trap-assisted tunnelling lagged a
        // step: at x.)
        if (lagged) mobilities(&x);
        if (tat && carriers) {
            const Mesh3& mesh = a_s.mesh();
            const Space3& sp = *field().space;
            std::vector<double> sum(std::size_t(a_n), 0), w(std::size_t(a_n), 0);
            for (int e = 0; e < int(a_prisms.size()); ++e) {
                if (a_prisms[std::size_t(e)].domain < 0) continue;
                PrismShape s;
                prismShapeAt(1, 1.0 / 3, 1.0 / 3, 0.5, s);
                const Space3::Map mp = sp.map(e, 1.0 / 3, 1.0 / 3, 0.5);
                double g3[3] = {0, 0, 0};
                const int* dofs = sp.dofs(e);
                for (int k = 0; k < 6; ++k) {
                    double g[3];
                    Space3::gradient(mp, s.dxi[k], s.deta[k], s.dzeta[k], g);
                    for (int c = 0; c < 3; ++c) g3[c] += g[c] * x(iV(dofs[k]));
                }
                const double E = std::sqrt(g3[0] * g3[0] + g3[1] * g3[1] + g3[2] * g3[2]), vol = a_prisms[std::size_t(e)].volume;
                for (int k = 0; k < 6; ++k) {
                    // (A node's own domain's prisms alone: a heterojunction's
                    // node its own side's field, 2D's.)
                    if (a_nodeMedium[std::size_t(dofs[k])] != a_prisms[std::size_t(e)].domain) continue;
                    sum[std::size_t(dofs[k])] += vol * E;
                    w[std::size_t(dofs[k])] += vol;
                }
            }
            (void)mesh;
            for (int i = 0; i < a_n; ++i) a_field[std::size_t(i)] = w[std::size_t(i)] > 0 ? sum[std::size_t(i)] / w[std::size_t(i)] : 0.0;
        }
        Eigen::VectorXd Fn;
        std::vector<Eigen::Triplet<double>> trip;
        assemble(x, Fn, &trip, carriers);
        heldValues(heldAt(x, applied, carriers), held);
        // The circuits' rows, from the contacts' rows as assembled; held
        // unknowns: their rows x - value (a circuit's contact's potential
        // its own voltage's, Vc + its offset).
        Eigen::VectorXd F = Eigen::VectorXd::Zero(N);
        F.head(3 * a_n) = Fn;
        std::vector<Eigen::Triplet<double>> kept;
        kept.reserve(trip.size());
        circuitRows(x, Fn, &trip, applied, carriers, F, &kept);
        std::vector<char> fixed(std::size_t(3 * a_n), 0);
        for (int k = 0; k < 3 * a_n; ++k)
            if (!std::isnan(held[std::size_t(k)])) {
                fixed[std::size_t(k)] = 1;
                F(k) = x(k) - held[std::size_t(k)];
            }
        for (const auto& t : trip)
            if (!fixed[std::size_t(t.row())]) kept.push_back(t);
        for (int k = 0; k < 3 * a_n; ++k)
            if (fixed[std::size_t(k)]) kept.emplace_back(k, k, 1.0);
        if (carriers)
            for (int c = 0; c < int(a_contacts.size()); ++c) {
                if (a_contacts[std::size_t(c)].iC < 0) continue;
                for (const auto& [i, area] : a_contacts[std::size_t(c)].nodes) {
                    (void)area;
                    if (a_heldKind[std::size_t(i)] == c) kept.emplace_back(iV(i), a_contacts[std::size_t(c)].iC, -1.0);
                }
            }
        Eigen::SparseMatrix<double> Jm(N, N);
        Jm.setFromTriplets(kept.begin(), kept.end());
        // Each carrier's column scaled by its value - its step δn/n: an
        // electron's 16 decades under a MOS capacitor's inversion layer
        // were the LU's absolute error, 1e-5 of it at best, else.
        Eigen::VectorXd cols = Eigen::VectorXd::Ones(N);
        if (carriers)
            for (int i = 0; i < a_n; ++i)
                if (a_nodeMedium[std::size_t(i)] >= 0)
                    for (int k : {iN(i), iP(i)}) cols(k) = std::max(std::abs(x(k)), 1e-300);
        Jm = Jm * cols.asDiagonal();
        // Each row scaled by its largest entry.
        Eigen::VectorXd scale = Eigen::VectorXd::Zero(N);
        for (int c = 0; c < Jm.outerSize(); ++c)
            for (Eigen::SparseMatrix<double>::InnerIterator it2(Jm, c); it2; ++it2) scale(it2.row()) = std::max(scale(it2.row()), std::abs(it2.value()));
        for (int k = 0; k < N; ++k) scale(k) = scale(k) > 0 ? 1 / scale(k) : 1;
        Jm = scale.asDiagonal() * Jm;
        const Eigen::VectorXd rhs = -(scale.asDiagonal() * F);
        SparseLu<double> lu;
        lu.compute(Jm);
        if (lu.info() != Eigen::Success) {
            *why = tr("%1: Newton's matrix could not be factorized (%2)").arg(a_node->name(), QString::fromStdString(lu.lastErrorMessage()));
            return false;
        }
        // (One step of iterative refinement: the pivots of a system of
        // carriers over many decades lose digits.)
        Eigen::VectorXd y = lu.solve(rhs);
        y += lu.solve(rhs - Jm * y);
        Eigen::VectorXd dx = cols.asDiagonal() * y;
        if (!dx.allFinite()) {
            *why = tr("%1: Newton's step is not finite").arg(a_node->name());
            return false;
        }
        // Damped: the potential's step at most 10 Vt; a carrier's that would
        // fall under half of it in its logarithm, n e^(δn/n), positive
        // however far it goes, at most a factor e^5 a step (a step shortened
        // whole to keep one positive stalled a MOS capacitor's inverting
        // electrons, over 16 decades).
        double t = 1, dvMax = 0;
        for (int i = 0; i < a_n; ++i) dvMax = std::max(dvMax, std::abs(dx(iV(i))));
        for (int k = 3 * a_n; k < N; ++k) dvMax = std::max(dvMax, std::abs(dx(k)));
        if (dvMax > 10 * a_Vt) t = 10 * a_Vt / dvMax;
        bool clamped = false;
        const auto stepped = [&](double tt, bool* clampedOut) {
            Eigen::VectorXd xc = x;
            *clampedOut = false;
            for (int k = 3 * a_n; k < N; ++k) xc(k) += tt * dx(k);
            for (int i = 0; i < a_n; ++i) {
                xc(iV(i)) += tt * dx(iV(i));
                if (!carriers || a_nodeMedium[std::size_t(i)] < 0) continue;
                for (int k : {iN(i), iP(i)}) {
                    if (!(xc(k) > 0)) {
                        xc(k) = std::max(xc(k) + tt * dx(k), 1e-300);
                        continue;
                    }
                    const double r = tt * dx(k) / xc(k);
                    if (r >= -0.5) xc(k) += tt * dx(k);
                    else {
                        *clampedOut = true;
                        xc(k) *= std::exp(std::max(r, -5.0));
                    }
                }
            }
            return xc;
        };
        // The residual, scaled as the rows are, held rows their misses.
        const auto residual = [&](const Eigen::VectorXd& xc) {
            Eigen::VectorXd Fc;
            assemble(xc, Fc, nullptr, carriers);
            std::vector<double> heldc;
            heldValues(heldAt(xc, applied, carriers), heldc);
            Eigen::VectorXd G = Eigen::VectorXd::Zero(N);
            G.head(3 * a_n) = Fc;
            circuitRows(xc, Fc, nullptr, applied, carriers, G, nullptr);
            for (int k = 0; k < 3 * a_n; ++k)
                if (fixed[std::size_t(k)]) G(k) = xc(k) - heldc[std::size_t(k)];
            return (scale.asDiagonal() * G).norm();
        };
        // A line search: the step halved till the residual falls (Newton's
        // full step overshoots where a carrier grows as e^(V/Vt)).
        const double r0 = (scale.asDiagonal() * F).norm();
        Eigen::VectorXd xc = stepped(t, &clamped);
        for (int halving = 0; halving < 12; ++halving) {
            const double rc = residual(xc);
            if (rc <= (1 - 1e-4 * t) * r0 || r0 == 0) break;
            t /= 2;
            xc = stepped(t, &clamped);
        }
        x = xc;
        // Converged: the potential's step under 1e-9 V, each carrier's under
        // 1e-6 of it.
        double rel = 0;
        for (int k = 3 * a_n; k < N; ++k)
            if (std::abs(dx(k)) / 1e-9 > rel) {
                rel = std::abs(dx(k)) / 1e-9;
                worst = k;
            }
        for (int i = 0; i < a_n; ++i) {
            if (std::abs(dx(iV(i))) / 1e-9 > rel) {
                rel = std::abs(dx(iV(i))) / 1e-9;
                worst = iV(i);
            }
            if (carriers && a_nodeMedium[std::size_t(i)] >= 0)
                for (int k : {iN(i), iP(i)})
                    if (std::abs(dx(k)) / std::max(std::abs(x(k)), 1e-300) / 1e-6 > rel) {
                        rel = std::abs(dx(k)) / std::max(std::abs(x(k)), 1e-300) / 1e-6;
                        worst = k;
                    }
        }
        lastRel = rel;
        lastT = t;
        if (steps) *steps = it + 1;
        // (Or its residual at rounding's floor - each row's over its largest
        // coupling under 1e-13.) With lagged mobilities, not before a second
        // step.
        // (Its floor where the line search could no longer lower it.)
        // One more step once it is - Newton's own, quadratic: a carrier at
        // 1e-6 of its tolerance left the free nodes' fluxes that far from
        // balance, a diode's anode's current 2e-6 off its cathode's.
        if (((t == 1 && !clamped && rel < 1) || (r0 < 1e-13 && t < 1)) && (!lagged || it > 0)) {
            if (polished || r0 < 1e-13) return true;
            polished = true;
        }
    }
    *why = tr("%1: Newton's method did not converge in %2 steps (its last change %3 of its tolerance, of %4 at node %5, step %6)")
               .arg(a_node->name())
               .arg(maxSteps)
               .arg(lastRel, 0, 'g', 3)
               .arg(worst >= 3 * a_n ? tr("a contact's own voltage") : worst % 3 == 0 ? QStringLiteral("V") : worst % 3 == 1 ? QStringLiteral("n") : QStringLiteral("p"))
               .arg(worst >= 3 * a_n ? worst - 3 * a_n : worst / 3)
               .arg(lastT, 0, 'g', 3);
    return false;
}

// ------------------------------------------------------------------ Solve

bool SemiconductorProblem3::rampTo(Eigen::VectorXd& x, const std::vector<double>& target, const Options3& o, QString* error)
{
    // The contacts' voltages raised together from those last solved, in
    // steps: the next one longer after an easy one, halved where one fails.
    const QString name = a_node->name();
    std::vector<double> from(target.size(), 0), applied(target.size(), 0);
    for (std::size_t c = 0; c < target.size(); ++c) from[c] = a_contacts[c].applied;
    double most = 0;
    for (std::size_t c = 0; c < target.size(); ++c) most = std::max(most, std::abs(target[c] - from[c]));
    double done = 0, step = most > 0 ? std::min(1.0, 0.1 / most) : 1.0;
    int steps = 0, walked = 0, newtons = 0, failed = 0;
    QString why;
    if (most == 0) {
        if (!newton(x, from, true, 50, &steps, &why)) {
            *error = why;
            return false;
        }
        return true;
    }
    while (done < 1) {
        if (o.cancelled && o.cancelled()) {
            *error = tr("cancelled");
            return false;
        }
        const double next = std::min(1.0, done + step);
        for (std::size_t c = 0; c < target.size(); ++c) applied[c] = from[c] + next * (target[c] - from[c]);
        Eigen::VectorXd trial = x;
        const bool ok = newton(trial, applied, true, 30, &steps, &why);
        newtons += steps;
        if (ok) {
            x = trial;
            done = next;
            ++walked;
            if (steps < 8) step = std::min(step * 1.5, 1.0);
        } else {
            ++failed;
            step /= 2;
            if (step * most < 1e-5) {
                *error = tr("%1: the contacts' voltages could not be raised past %2 of theirs: %3").arg(name).arg(done, 0, 'g', 3).arg(why);
                return false;
            }
        }
    }
    a_s.log << tr("%1: the contacts' voltages raised in %2 steps (%3 halved), %4 Newton steps").arg(name).arg(walked).arg(failed).arg(newtons);
    return true;
}

bool SemiconductorProblem3::solve(const Options3& o, QString* error)
{
    Field3& f = field();
    const QString name = a_node->name();
    QElapsedTimer clock;
    clock.start();
    std::vector<double> target(a_contacts.size(), 0);
    for (std::size_t c = 0; c < a_contacts.size(); ++c) {
        const Expression& v = a_contacts[c].voltage;
        std::vector<double> vals(std::size_t(a_s.pointScope().slotCount()), NaN);
        for (int slot : v.variables())
            if (a_s.definitions()[std::size_t(slot)].kind == Solution3::VariableDef::Time) vals[std::size_t(slot)] = a_s.time();
        target[c] = v.isConstant() ? v.constant() : v.isValid() ? v.eval(vals.data()) : 0;
        if (!std::isfinite(target[c])) {
            *error = tr("%1: contact %2's voltage is not a number").arg(name, a_contacts[c].name);
            return false;
        }
    }
    // The lattice's temperature at each node, where it varies (a heat
    // physics' field, solved in turn with this one), and the bands and
    // mobilities of it.
    if (a_TLexpr.isValid()) {
        PointEvaluator3 ev(a_s);
        const Mesh3& mesh = a_s.mesh();
        std::vector<double> sum(std::size_t(a_n), 0), cnt(std::size_t(a_n), 0);
        static const double corner[6][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}};
        for (int e = 0; e < int(mesh.prisms.size()); ++e) {
            if (a_prisms[std::size_t(e)].domain < 0) continue;
            for (int k = 0; k < 6; ++k) {
                ev.at(e, corner[k][0], corner[k][1], corner[k][2]);
                const double T = ev.eval(a_TLexpr);
                if (!std::isfinite(T) || !(T > 0)) continue;
                const int i = mesh.prisms[std::size_t(e)][std::size_t(k)];
                sum[std::size_t(i)] += T;
                cnt[std::size_t(i)] += 1;
            }
        }
        for (int i = 0; i < a_n; ++i) a_TL[std::size_t(i)] = cnt[std::size_t(i)] > 0 ? sum[std::size_t(i)] / cnt[std::size_t(i)] : a_T;
        nodeBands();
        mobilities(nullptr);
    }
    a_mass = o.mass;
    a_history = o.history;
    // Generation of t at this time.
    if (!a_genTimed.empty()) {
        std::vector<double> timed(std::size_t(a_n), 0), timedPhoton(std::size_t(a_n), 0), timedGap(std::size_t(a_n), 0);
        for (const TimedGeneration& tg : a_genTimed) {
            std::vector<double> vals(std::size_t(a_s.pointScope().slotCount()), NaN);
            const Vec3 xi = f.space->point(tg.node);
            for (int slot : tg.G.variables()) {
                const auto& def = a_s.definitions()[std::size_t(slot)];
                using K = Solution3::VariableDef::Kind;
                vals[std::size_t(slot)] = def.kind == K::X ? xi.x : def.kind == K::Y ? xi.y : def.kind == K::Z ? xi.z : def.kind == K::Time ? a_s.time() : NaN;
            }
            const double g = tg.G.eval(vals.data());
            if (!std::isfinite(g)) continue;
            timed[std::size_t(tg.node)] += tg.share * g;
            if (std::isfinite(tg.photon)) timedPhoton[std::size_t(tg.node)] += tg.share * g * tg.photon;
            else timedGap[std::size_t(tg.node)] += tg.share * g;
        }
        if (a_genBase.empty()) {
            a_genBase = a_generation;
            a_genPhotonBase = a_genPhoton;
            a_genGapBase = a_genGapRate;
        }
        for (int i = 0; i < a_n; ++i) {
            a_generation[std::size_t(i)] = a_genBase[std::size_t(i)] + timed[std::size_t(i)];
            a_genPhoton[std::size_t(i)] = a_genPhotonBase[std::size_t(i)] + timedPhoton[std::size_t(i)];
            a_genGapRate[std::size_t(i)] = a_genGapBase[std::size_t(i)] + timedGap[std::size_t(i)];
        }
    }
    if (!a_optical.empty() && !opticalGeneration(error)) return false;
    // A step in time: from the state as it was, the contacts at this time's
    // voltages, at once.
    if (a_mass != 0 && !a_solvedOnce) {
        // (A step in time's start: the state at rest first.)
        Options3 rest = o;
        rest.mass = 0;
        rest.history = nullptr;
        if (!solve(rest, error)) return false;
        a_mass = o.mass;
        a_history = o.history;
    }
    if (a_mass != 0 && a_solvedOnce) {
        Eigen::VectorXd x = a_x;
        int steps = 0;
        QString why;
        if (!newton(x, target, true, 50, &steps, &why)) {
            *error = tr("%1, at t = %2: %3").arg(name).arg(a_s.time()).arg(why);
            return false;
        }
        for (std::size_t c = 0; c < target.size(); ++c) a_contacts[c].applied = target[c];
        store(x);
        return true;
    }
    Eigen::VectorXd x = Eigen::VectorXd::Zero(a_unknowns);
    for (std::size_t c = 0; c < a_contacts.size(); ++c) a_contacts[c].applied = 0;
    // From equilibrium: the neutral carriers' potential, Poisson's at rest,
    // then the carriers.
    for (int i = 0; i < a_n; ++i)
        if (a_nodeMedium[std::size_t(i)] >= 0) x(iV(i)) = a_neutralU[std::size_t(i)];
    int steps = 0;
    QString why;
    a_fermi = 0;
    std::vector<double> zero(a_contacts.size(), 0);
    if (!newton(x, zero, false, 100, &steps, &why)) {
        *error = tr("%1, at equilibrium: %2").arg(name, why);
        return false;
    }
    a_s.log << tr("%1: %2 nodes, %3 unknowns; equilibrium in %4 Newton steps").arg(name).arg(a_n).arg(a_unknowns).arg(steps);
    const auto fill = [&](double fermi) {
        for (int i = 0; i < a_n; ++i) {
            if (a_nodeMedium[std::size_t(i)] < 0) continue;
            double n, p, dn, dp;
            carriersAt(i, x(iV(i)) - fermi, &n, &p, &dn, &dp);
            x(iN(i)) = n / a_C0;
            x(iP(i)) = p / a_C0;
        }
    };
    fill(0);
    // At equilibrium all the way - every contact but the gates at one
    // voltage, nothing generated, not in time: no current, the carriers of
    // one Fermi level, Poisson's equation alone (a MOS capacitor's C-V: its
    // inversion layer's electrons, over 16 decades, left Newton's method on
    // the carriers at rounding's floor).
    bool equilibrium = a_mass == 0;
    double common = NaN;
    for (std::size_t c = 0; c < a_contacts.size(); ++c) {
        if (a_contacts[c].kind == Contact::Gate) continue;
        if (std::isnan(common)) common = target[c];
        else if (std::abs(target[c] - common) > 1e-12) equilibrium = false;
    }
    for (double g : a_generation) equilibrium = equilibrium && g == 0;
    equilibrium = equilibrium && a_genTimed.empty();
    for (const Medium& m : a_media) equilibrium = equilibrium && !(m.semiconductor && (m.ionization || m.tunnelling));
    if (equilibrium && !std::isnan(common)) {
        a_fermi = common;
        const Eigen::VectorXd start = x;
        if (!newton(x, target, false, 200, &steps, &why)) {
            x = start;
            double done = 0, step = 0.25;
            std::vector<double> part(target.size(), 0);
            while (done < 1) {
                const double next = std::min(1.0, done + step);
                for (std::size_t c = 0; c < target.size(); ++c) part[c] = next * target[c];
                a_fermi = next * common;
                Eigen::VectorXd trial = x;
                if (newton(trial, part, false, 100, &steps, &why)) {
                    x = trial;
                    done = next;
                    step = std::min(2 * step, 1.0);
                } else if ((step /= 2) < 1e-6) {
                    *error = tr("%1, at equilibrium: %2").arg(name, why);
                    return false;
                }
            }
        }
        fill(a_fermi);
        a_s.log << tr("%1: at equilibrium (no current), Poisson's equation alone, %2 s").arg(name).arg(clock.elapsed() / 1000.0, 0, 'f', 2);
        for (std::size_t c = 0; c < target.size(); ++c) a_contacts[c].applied = target[c];
        a_fermi = 0;
        store(x);
        for (Contact& c : a_contacts) c.current = 0;
        return true;
    }
    // (Hot carriers: walked with the field's mobilities, their temperatures
    // anew at its end - theirs and their mobilities each other's, a
    // temperature some of the way each step: up to 300 of them.)
    a_energyActive = false;
    bool hotMedia = false;
    for (const Medium& m : a_media) hotMedia = hotMedia || (m.semiconductor && m.energy);
    if (!newton(x, zero, true, 50, &steps, &why)) {
        a_energyActive = true;
        *error = tr("%1, at equilibrium with its carriers: %2").arg(name, why);
        return false;
    }
    if (!rampTo(x, target, o, error)) {
        a_energyActive = true;
        return false;
    }
    a_energyActive = true;
    if (hotMedia) {
        for (auto* v : {&a_Tn, &a_Tp, &a_TnWas, &a_TpWas, &a_dTnWas, &a_dTpWas}) v->clear();
        a_omegaT[0] = a_omegaT[1] = 1;
        a_residualT[0] = a_residualT[1] = 0;
        if (!newton(x, target, true, 300, &steps, &why)) {
            *error = tr("%1: its hot carriers' temperatures did not settle: %2").arg(name, why);
            return false;
        }
        a_s.log << tr("%1: hot carriers' temperatures in %2 Newton steps").arg(name).arg(steps);
    }
    a_s.log << tr("%1: the contacts' voltages raised, %2 s").arg(name).arg(clock.elapsed() / 1000.0, 0, 'f', 2);
    for (std::size_t c = 0; c < target.size(); ++c) a_contacts[c].applied = target[c];
    store(x);
    (void)f;
    return true;
}

void SemiconductorProblem3::store(const Eigen::VectorXd& x)
{
    Field3& f = field();
    a_x = x;
    a_solvedOnce = true;
    contactResults(x);
    // The carriers' heat, by node: each edge's currents times their
    // quasi-Fermi potentials' drops, half to each end, and the pairs' recombination (less their
    // generation) q (R - G)(φp - φn) in each box - their sum every
    // contact's I V - over the node's box.
    std::vector<double> heat(std::size_t(a_n), 0.0);
    const double Vt = a_Vt, C0 = a_C0;
    std::vector<double> phin(std::size_t(a_n), 0), phip(std::size_t(a_n), 0);
    for (int i = 0; i < a_n; ++i) {
        if (a_nodeMedium[std::size_t(i)] < 0) continue;
        const double n = std::max(x(iN(i)) * C0, 1e-300), p = std::max(x(iP(i)) * C0, 1e-300);
        const Medium& m = a_media[std::size_t(a_nodeMedium[std::size_t(i)])];
        double un = Vt * (std::log(n) - a_an[std::size_t(i)]), up = -Vt * (std::log(p) - a_ap[std::size_t(i)]);
        if (m.fermi) {
            const auto& fh = detail::fermiHalf();
            un = Vt * (fh.inverse(n / m.Nc) - a_an[std::size_t(i)] + std::log(m.Nc));
            up = -Vt * (fh.inverse(p / m.Nv) - a_ap[std::size_t(i)] + std::log(m.Nv));
        }
        phin[std::size_t(i)] = x(iV(i)) - un;
        phip[std::size_t(i)] = x(iV(i)) - up;
    }
    std::vector<double> lgN, lgP;
    degeneracy(x, lgN, lgP);
    for (const Edge& e : a_edges) {
        if (e.wn <= 0 || a_nodeMedium[std::size_t(e.i)] < 0 || a_nodeMedium[std::size_t(e.j)] < 0) continue;
        const int i = e.i, j = e.j;
        const EdgeState st = edgeState(e, x, lgN, lgP);
        const double gn = e.wn * Vt * (st.ni * bern(-st.Dn) - st.nj * bern(st.Dn)) * C0;
        const double gp = e.wp * Vt * (st.pi * bern(st.Dp) - st.pj * bern(-st.Dp)) * C0;
        // (Each carrier's current times its own quasi-Fermi potential's drop
        // - 2D's: the potential's drop took a photodiode's ∫ semi.H 3 % over
        // the light's power, its currents' divergence the generation.)
        const double P = -Q * gn * (phin[std::size_t(i)] - phin[std::size_t(j)]) + Q * gp * (phip[std::size_t(i)] - phip[std::size_t(j)]);
        heat[std::size_t(i)] += 0.5 * P;
        heat[std::size_t(j)] += 0.5 * P;
    }
    for (int i = 0; i < a_n; ++i) {
        if (a_nodeMedium[std::size_t(i)] < 0) continue;
        double dn, dp;
        const double RV = recombination(i, x(iN(i)) * C0, x(iP(i)) * C0, a_field[std::size_t(i)], &dn, &dp) * a_recBox[std::size_t(i)]
                          - a_generation[std::size_t(i)] * a_box[std::size_t(i)];
        heat[std::size_t(i)] += Q * RV * (phip[std::size_t(i)] - phin[std::size_t(i)]);
        // (The generation's pairs' energy as made - a photon's, or the
        // gap's: what is not their quasi-Fermi levels' split heats.)
        const Medium& gm = a_media[std::size_t(a_nodeMedium[std::size_t(i)])];
        const double made = (a_genPhoton.empty() ? 0.0 : a_genPhoton[std::size_t(i)])
                            + (a_genGapRate.empty() ? 0.0 : a_genGapRate[std::size_t(i)] * (gm.Eg - a_gap[std::size_t(i)]));
        heat[std::size_t(i)] += made * a_box[std::size_t(i)];
    }
    f.values.assign(std::size_t(Components * a_n), 0.0);
    const double kT = KB * a_T;
    for (int i = 0; i < a_n; ++i) {
        f.values[std::size_t(CV * a_n + i)] = x(iV(i));
        f.values[std::size_t(CNd * a_n + i)] = a_Nd[std::size_t(i)];
        f.values[std::size_t(CNa * a_n + i)] = a_Na[std::size_t(i)];
        const int d = a_nodeMedium[std::size_t(i)];
        if (d < 0) continue;
        const Medium& m = a_media[std::size_t(d)];
        f.values[std::size_t(Cn * a_n + i)] = x(iN(i)) * C0;
        f.values[std::size_t(Cp * a_n + i)] = x(iP(i)) * C0;
        f.values[std::size_t(CH * a_n + i)] = a_box[std::size_t(i)] > 0 ? heat[std::size_t(i)] / a_box[std::size_t(i)] : 0.0;
        // The bands, against the reference's intrinsic level at V = 0: Ec
        // = -qV + dEcr - (χ - χr) - narrowing/2, Ev = Ec - Eg + narrowing.
        const double Ec = -Q * x(iV(i)) + a_refDEc - (m.chi - a_refChi) - a_gap[std::size_t(i)] / 2;
        f.values[std::size_t(CEc * a_n + i)] = Ec;
        f.values[std::size_t(CEv * a_n + i)] = Ec - m.Eg + a_gap[std::size_t(i)];
        f.values[std::size_t(CEfn * a_n + i)] = -Q * phin[std::size_t(i)];
        f.values[std::size_t(CEfp * a_n + i)] = -Q * phip[std::size_t(i)];
        double dn, dp;
        f.values[std::size_t(CR * a_n + i)] = recombination(i, x(iN(i)) * C0, x(iP(i)) * C0, a_field[std::size_t(i)], &dn, &dp);
        f.values[std::size_t(CTn * a_n + i)] = a_Tn.size() == std::size_t(a_n) ? a_Tn[std::size_t(i)] : a_TL[std::size_t(i)];
        f.values[std::size_t(CTp * a_n + i)] = a_Tp.size() == std::size_t(a_n) ? a_Tp[std::size_t(i)] : a_TL[std::size_t(i)];
        (void)kT;
    }
    // Each contact's own voltage, its charge and a bias tee's current at
    // rest, after the nodes' (a step in time's history of them).
    f.values.resize(std::size_t(Components * a_n) + 3 * a_contacts.size(), 0.0);
    for (std::size_t c = 0; c < a_contacts.size(); ++c) {
        f.values[std::size_t(Components * a_n) + 3 * c] = a_contacts[c].own;
        f.values[std::size_t(Components * a_n) + 3 * c + 1] = a_contacts[c].charge;
        f.values[std::size_t(Components * a_n) + 3 * c + 2] = a_contacts[c].teeCurrent;
    }
    // The prisms' mobilities as solved (the variables' currents').
    Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    auto& mn = coef.elements[QStringLiteral("mun")];
    auto& mp = coef.elements[QStringLiteral("mup")];
    mn.assign(a_prisms.size(), NaN);
    mp.assign(a_prisms.size(), NaN);
    for (std::size_t e = 0; e < a_prisms.size(); ++e)
        if (a_prisms[e].domain >= 0) {
            mn[e] = a_prisms[e].mun;
            mp[e] = a_prisms[e].mup;
        }
    f.solved = true;
}

double SemiconductorProblem3::contactCurrent(int c, const Eigen::VectorXd& x, const Eigen::VectorXd& F, const std::vector<Eigen::Triplet<double>>* trip,
                                             std::map<int, double>* dI) const
{
    // Its current into the device, q (holes - electrons) its held nodes
    // give the device (their boxes' equations' residuals: what flows out of
    // them and recombines in them) - a Schottky contact's its carriers'
    // emission over the barrier, q (vn (n - nB) - vp (p - pB)) -, in time
    // with its charge's change (Poisson's residual at its nodes).
    const Contact& ct = a_contacts[std::size_t(c)];
    double I = 0, charge = 0;
    std::vector<std::pair<int, double>> rows;   // (row, its weight in I)
    for (const auto& [i, area] : ct.nodes) {
        if (a_heldKind[std::size_t(i)] != c) continue;
        charge += F(iV(i));
        if (inTime()) rows.emplace_back(iV(i), a_mass);
        const int d = a_nodeMedium[std::size_t(i)];
        if (d < 0) continue;
        if (ct.kind == Contact::Schottky) {
            const Medium& m = a_media[std::size_t(d)];
            const double nB = m.Nc * std::exp(-(ct.phiB - a_gap[std::size_t(i)] / 2) / (KB * a_T));
            const double pB = std::exp(a_an[std::size_t(i)] + a_ap[std::size_t(i)]) / nB;
            I += Q * area * (ct.vn * (x(iN(i)) * a_C0 - nB) - ct.vp * (x(iP(i)) * a_C0 - pB));
            if (dI) {
                (*dI)[iN(i)] += Q * area * ct.vn * a_C0;
                (*dI)[iP(i)] -= Q * area * ct.vp * a_C0;
            }
        } else {
            I += Q * a_C0 * (F(iP(i)) - F(iN(i)));
            rows.emplace_back(iP(i), Q * a_C0);
            rows.emplace_back(iN(i), -Q * a_C0);
        }
    }
    if (inTime()) I += a_mass * charge - historyOf(c, 1);
    if (dI && trip && !rows.empty()) {
        std::vector<double> w(std::size_t(3 * a_n), 0.0);
        for (const auto& [r, v] : rows) w[std::size_t(r)] += v;
        for (const auto& t : *trip)
            if (const double wr = w[std::size_t(t.row())]; wr != 0) (*dI)[t.col()] += wr * t.value();
    }
    return I;
}

void SemiconductorProblem3::circuitRows(const Eigen::VectorXd& x, const Eigen::VectorXd& F, const std::vector<Eigen::Triplet<double>>* trip,
                                        const std::vector<double>& applied, bool carriers, Eigen::VectorXd& G, std::vector<Eigen::Triplet<double>>* J) const
{
    for (int c = 0; c < int(a_contacts.size()); ++c) {
        const Contact& ct = a_contacts[std::size_t(c)];
        if (ct.iC < 0) continue;
        const double Vc = x(ct.iC);
        if (!carriers) {
            // (Poisson's equation alone: no current, its own voltage its
            // source's.)
            G(ct.iC) = Vc - applied[std::size_t(c)];
            if (J) J->emplace_back(ct.iC, ct.iC, 1.0);
            continue;
        }
        std::map<int, double> dI;
        const double I = contactCurrent(c, x, F, trip, J ? &dI : nullptr);
        double Ic = 0, dIc = 0;
        if (inTime() && ct.Cp > 0) {
            Ic = ct.Cp * (a_mass * Vc - historyOf(c, 0));
            dIc = ct.Cp * a_mass;
        }
        const double R = inTime() ? ct.Rs + ct.RL : ct.restR();
        double g = Vc - applied[std::size_t(c)] + R * (I + Ic);
        if (inTime() && ct.circuit == Contact::BiasTee) g -= ct.RL * ct.teeCurrent;
        G(ct.iC) = g;
        if (!J) continue;
        J->emplace_back(ct.iC, ct.iC, 1 + R * dIc);
        if (R > 0)
            for (const auto& [col, d] : dI)
                if (d != 0) J->emplace_back(ct.iC, col, R * d);
    }
}

std::vector<std::vector<double>> SemiconductorProblem3::currentWeights(const Eigen::VectorXd& x) const
{
    const std::size_t nc = a_contacts.size();
    std::vector<std::vector<double>> weight(nc);
    std::vector<int> held(std::size_t(a_n), -1), local(std::size_t(a_n), -1), columns(nc, -1);
    int carriersHeld = 0;
    for (std::size_t k = 0; k < nc; ++k) {
        if (a_contacts[k].kind == Contact::Gate) continue;
        columns[k] = carriersHeld++;
        for (const auto& [i, area] : a_contacts[k].nodes) {
            (void)area;
            if (a_heldKind[std::size_t(i)] == int(k)) held[std::size_t(i)] = int(k);
        }
    }
    int free = 0;
    for (int i = 0; i < a_n; ++i)
        if (a_nodeMedium[std::size_t(i)] >= 0 && held[std::size_t(i)] < 0) local[std::size_t(i)] = free++;
    // Each edge's conductance (its currents' change with the quasi-Fermi
    // potentials' difference), relative to the largest and at least 1e-12
    // of it.
    std::vector<double> g(a_edges.size(), 0.0);
    double largest = 0;
    std::vector<double> lgN, lgP;
    degeneracy(x, lgN, lgP);
    for (std::size_t k = 0; k < a_edges.size(); ++k) {
        const Edge& e = a_edges[k];
        if (e.wn <= 0 || a_nodeMedium[std::size_t(e.i)] < 0 || a_nodeMedium[std::size_t(e.j)] < 0) continue;
        const EdgeState st = edgeState(e, x, lgN, lgP);
        g[k] = e.wn * 0.5 * (st.ni * bern(-st.Dn) + st.nj * bern(st.Dn)) + e.wp * 0.5 * (st.pi * bern(st.Dp) + st.pj * bern(-st.Dp));
        if (std::isfinite(g[k])) largest = std::max(largest, g[k]);
    }
    for (std::size_t k = 0; k < g.size(); ++k)
        g[k] = a_edges[k].wn > 0 && largest > 0 && std::isfinite(g[k]) ? std::max(g[k] / largest, 1e-12) : 0.0;
    Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(std::max(free, 1), std::max(carriersHeld, 1));
    if (free > 0 && carriersHeld > 0) {
        std::vector<Eigen::Triplet<double>> trip;
        for (std::size_t k = 0; k < a_edges.size(); ++k) {
            if (g[k] == 0) continue;
            const int i = a_edges[k].i, j = a_edges[k].j;
            const int li = local[std::size_t(i)], lj = local[std::size_t(j)];
            if (li >= 0) trip.emplace_back(li, li, g[k]);
            if (lj >= 0) trip.emplace_back(lj, lj, g[k]);
            if (li >= 0 && lj >= 0) {
                trip.emplace_back(li, lj, -g[k]);
                trip.emplace_back(lj, li, -g[k]);
            } else if (li >= 0 && held[std::size_t(j)] >= 0) rhs(li, columns[std::size_t(held[std::size_t(j)])]) += g[k];
            else if (lj >= 0 && held[std::size_t(i)] >= 0) rhs(lj, columns[std::size_t(held[std::size_t(i)])]) += g[k];
        }
        // (A little to nought everywhere: a region no contact reaches
        // weighed nought, not singular.)
        for (int k = 0; k < free; ++k) trip.emplace_back(k, k, 1e-14);
        Eigen::SparseMatrix<double> L(free, free);
        L.setFromTriplets(trip.begin(), trip.end());
        Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt(L);
        if (ldlt.info() == Eigen::Success) rhs = ldlt.solve(rhs);
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

void SemiconductorProblem3::contactResults(const Eigen::VectorXd& x)
{
    // Each contact's current and its charge (Poisson's residual at its held
    // nodes); behind a circuit its own voltage and Cp's current.
    Eigen::VectorXd F;
    assemble(x, F, nullptr, true);
    for (int c = 0; c < int(a_contacts.size()); ++c) {
        Contact& ct = a_contacts[std::size_t(c)];
        ct.charge = 0;
        for (const auto& [i, area] : ct.nodes) {
            (void)area;
            if (a_heldKind[std::size_t(i)] == c) ct.charge += F(iV(i));
        }
        ct.current = contactCurrent(c, x, F, nullptr, nullptr);
        ct.own = ct.iC >= 0 && x.size() > ct.iC ? x(ct.iC) : ct.applied;
        ct.capCurrent = inTime() && ct.iC >= 0 && ct.Cp > 0 ? ct.Cp * (a_mass * ct.own - historyOf(c, 0)) : 0.0;
    }
    // The currents as reported: each contact's weight's drop along the
    // edges times their currents (a magnetic field's turned ones too), the
    // carriers' change in time where it weighs, and in time its charge's -
    // not its rows' difference of large fluxes.
    const std::vector<std::vector<double>> weight = currentWeights(x);
    std::vector<double> lgN, lgP;
    degeneracy(x, lgN, lgP);
    std::vector<double> hallEdge;
    if (hall()) {
        const std::vector<double> zero(std::size_t(a_n), 0.0);
        Eigen::VectorXd Fh = Eigen::VectorXd::Zero(3 * a_n);
        hallFluxes(x, lgN, lgP, zero, zero, Fh, [](int, int, double) {}, &hallEdge);
    }
    for (int c = 0; c < int(a_contacts.size()); ++c) {
        Contact& ct = a_contacts[std::size_t(c)];
        const std::vector<double>& w = weight[std::size_t(c)];
        if (!w.empty()) {
            double I = 0;
            for (std::size_t m = 0; m < a_edges.size(); ++m) {
                const Edge& e = a_edges[m];
                const double dw = w[std::size_t(e.i)] - w[std::size_t(e.j)];
                if (dw == 0 || a_nodeMedium[std::size_t(e.i)] < 0 || a_nodeMedium[std::size_t(e.j)] < 0) continue;
                double gn = 0, gp = 0;
                if (e.wn > 0) {
                    const EdgeState st = edgeState(e, x, lgN, lgP);
                    gn = e.wn * a_Vt * (st.ni * bern(-st.Dn) - st.nj * bern(st.Dn));
                    gp = e.wp * a_Vt * (st.pi * bern(st.Dp) - st.pj * bern(-st.Dp));
                }
                // (An edge of no coupling of its own - an obtuse triangle's
                // cotangents summing to nought - still carries its prisms'
                // turned currents.)
                if (!hallEdge.empty()) {
                    gn += hallEdge[2 * m];
                    gp += hallEdge[2 * m + 1];
                }
                I += Q * a_C0 * (gp - gn) * dw;
            }
            if (inTime())
                for (int i = 0; i < a_n; ++i) {
                    if (w[std::size_t(i)] == 0 || a_nodeMedium[std::size_t(i)] < 0) continue;
                    const double dn = a_mass * x(iN(i)) * a_C0 - (*a_history)[std::size_t(Cn * a_n + i)];
                    const double dp = a_mass * x(iP(i)) * a_C0 - (*a_history)[std::size_t(Cp * a_n + i)];
                    I += w[std::size_t(i)] * Q * (a_boxP[std::size_t(i)] * dp - a_boxN[std::size_t(i)] * dn);
                }
            if (inTime()) I += a_mass * ct.charge - historyOf(c, 1);
            ct.current = I;
        }
        if (!inTime()) ct.teeCurrent = ct.current;
    }
}

bool SemiconductorProblem3::smallSignal(double frequency, std::vector<std::vector<std::complex<double>>>* Y, QString* error)
{
    using cd = std::complex<double>;
    if (!a_solvedOnce) {
        *error = tr("%1: a small-signal step follows a Stationary one (its solution the bias)").arg(a_node->name());
        return false;
    }
    const Eigen::VectorXd& x = a_x;
    const double omega = 2 * M_PI * frequency;
    // (The device's own admittance: its contacts held at their voltages as
    // solved - behind a circuit its own.)
    std::vector<double> applied(a_contacts.size());
    for (std::size_t c = 0; c < a_contacts.size(); ++c) applied[c] = a_contacts[c].iC >= 0 ? a_contacts[c].own : a_contacts[c].applied;
    std::vector<double> held;
    heldValues(applied, held);
    if (std::any_of(a_media.begin(), a_media.end(), [](const Medium& m) { return m.semiconductor && m.varies(); })) mobilities(&x);
    Eigen::VectorXd F;
    std::vector<Eigen::Triplet<double>> trip;
    const double mass = a_mass;
    a_mass = 0;
    assemble(x, F, &trip, true);
    a_mass = mass;
    Eigen::SparseMatrix<double> J(3 * a_n, 3 * a_n);
    J.setFromTriplets(trip.begin(), trip.end());
    // (J + jω M) δx = -J δheld: M box on the carriers' rows (their ∂n/∂t),
    // each contact's held unknowns moved by its volt.
    std::vector<char> fixed(std::size_t(3 * a_n), 0);
    for (int k = 0; k < 3 * a_n; ++k) fixed[std::size_t(k)] = !std::isnan(held[std::size_t(k)]);
    std::vector<Eigen::Triplet<cd>> ct;
    for (int c = 0; c < J.outerSize(); ++c)
        for (Eigen::SparseMatrix<double>::InnerIterator it(J, c); it; ++it)
            if (!fixed[std::size_t(it.row())]) ct.emplace_back(it.row(), it.col(), cd(it.value(), 0));
    for (int i = 0; i < a_n; ++i) {
        if (a_nodeMedium[std::size_t(i)] < 0) continue;
        if (!fixed[std::size_t(iN(i))]) ct.emplace_back(iN(i), iN(i), cd(0, omega * a_boxN[std::size_t(i)]));
        if (!fixed[std::size_t(iP(i))]) ct.emplace_back(iP(i), iP(i), cd(0, omega * a_boxP[std::size_t(i)]));
    }
    // Charged traps follow with their time: jω δf = cn (1 - f) δn - cp f δp
    // - D δf, so δf = (cn (1 - f) δn - cp f δp)/(D + jω); their charge q N
    // δf on Poisson's row, the electrons they take N (cn (1 - f) δn - cn (n
    // + n1) δf), the holes N (cp f δp + cp (p + p1) δf) - in the place of
    // the static changes assembled (to which they come at 0).
    for (const Surface& sf : a_surfaces) {
        if (!sf.charged) continue;
        for (const auto& [i, area] : sf.nodes) {
            if (a_nodeMedium[std::size_t(i)] < 0) continue;
            const Medium& m = a_media[std::size_t(a_nodeMedium[std::size_t(i)])];
            double gn = 1, gp = 1, dln = 0, dlp = 0;   // (n/γ per n; d ln γ per δx)
            if (m.fermi) {
                const auto& fh = detail::fermiHalf();
                for (int c = 0; c < 2; ++c) {
                    const double dens = std::max(x(c == 0 ? iN(i) : iP(i)) * a_C0, 1e-300), Nb = c == 0 ? m.Nc : m.Nv;
                    const double eta = fh.inverse(dens / Nb);
                    double Fv = 0, dF = 0;
                    fh.at(eta, &Fv, &dF);
                    (c == 0 ? gn : gp) = std::exp(eta) / Fv;
                    (c == 0 ? dln : dlp) = (dF / Fv - 1) / (Nb * dF) * a_C0;
                }
            }
            const double n = x(iN(i)) * a_C0 * gn, p = x(iP(i)) * a_C0 * gp;
            const double ni = std::exp(0.5 * (a_an[std::size_t(i)] + a_ap[std::size_t(i)]));
            for (const Surface::Level& lv : sf.levels) {
                const double n1 = ni * std::exp(lv.Et / (KB * a_T)), p1 = ni * std::exp(-lv.Et / (KB * a_T));
                const TrapState st = trapState(lv.N, sf.cn, sf.cp, n1, p1, n, p);
                const cd den(st.D, omega);
                const cd fn = sf.cn * (1 - st.f) / den, fp = -sf.cp * st.f / den;
                const cd dun = lv.N * (sf.cn * (1 - st.f) - sf.cn * (n + n1) * fn), dup = lv.N * (-sf.cn * (n + n1) * fp);
                const cd dvn = lv.N * (sf.cp * (p + p1) * fn), dvp = lv.N * (sf.cp * st.f + sf.cp * (p + p1) * fp);
                // (δ(n/γ) per δx, as assembled.)
                const double sn = gn * (a_C0 - x(iN(i)) * a_C0 * dln), sp2 = gp * (a_C0 - x(iP(i)) * a_C0 * dlp);
                if (!fixed[std::size_t(iV(i))]) {
                    ct.emplace_back(iV(i), iN(i), Q * lv.N * area * (fn - st.dfdn) * sn);
                    ct.emplace_back(iV(i), iP(i), Q * lv.N * area * (fp - st.dfdp) * sp2);
                }
                if (!fixed[std::size_t(iN(i))]) {
                    ct.emplace_back(iN(i), iN(i), area * (dun - st.dUdn) * sn / a_C0);
                    ct.emplace_back(iN(i), iP(i), area * (dup - st.dUdp) * sp2 / a_C0);
                }
                if (!fixed[std::size_t(iP(i))]) {
                    ct.emplace_back(iP(i), iN(i), area * (dvn - st.dUdn) * sn / a_C0);
                    ct.emplace_back(iP(i), iP(i), area * (dvp - st.dUdp) * sp2 / a_C0);
                }
            }
        }
    }
    for (int k = 0; k < 3 * a_n; ++k)
        if (fixed[std::size_t(k)]) ct.emplace_back(k, k, cd(1, 0));
    Eigen::SparseMatrix<cd> A(3 * a_n, 3 * a_n);
    A.setFromTriplets(ct.begin(), ct.end());
    SparseLu<cd> lu;
    lu.compute(A);
    if (lu.info() != Eigen::Success) {
        *error = tr("%1: the small-signal matrix could not be factorized").arg(a_node->name());
        return false;
    }
    const int nc = int(a_contacts.size());
    Y->assign(std::size_t(nc), std::vector<cd>(std::size_t(nc), cd(0, 0)));
    for (int l = 0; l < nc; ++l) {
        // Contact l's volt: its held potentials (and nothing else held) up 1 V.
        Eigen::VectorXcd dheld = Eigen::VectorXcd::Zero(3 * a_n);
        for (const auto& [i, area] : a_contacts[std::size_t(l)].nodes) {
            (void)area;
            if (a_heldKind[std::size_t(i)] == l) dheld(iV(i)) = 1;
        }
        // (The free rows' whole: the held unknowns' volt is in δx itself.)
        Eigen::VectorXcd b = Eigen::VectorXcd::Zero(3 * a_n);
        for (int k = 0; k < 3 * a_n; ++k)
            if (fixed[std::size_t(k)]) b(k) = dheld(k);
        const Eigen::VectorXcd dx = lu.solve(b);
        // Each contact's current's change: its rows' (conduction, and the
        // carriers' own ∂/∂t at its held nodes) and jω its charge's.
        const Eigen::VectorXcd dF = J.cast<cd>() * dx;
        for (int k = 0; k < nc; ++k) {
            const Contact& c = a_contacts[std::size_t(k)];
            cd dI(0, 0), dQ(0, 0);
            for (const auto& [i, area] : c.nodes) {
                if (a_heldKind[std::size_t(i)] != k) continue;
                dQ += dF(iV(i));
                if (a_nodeMedium[std::size_t(i)] < 0) continue;
                if (c.kind == Contact::Schottky) dI += Q * area * a_C0 * (c.vn * dx(iN(i)) - c.vp * dx(iP(i)));
                else dI += Q * a_C0 * (dF(iP(i)) + cd(0, omega * a_boxP[std::size_t(i)]) * dx(iP(i)) - dF(iN(i)) - cd(0, omega * a_boxN[std::size_t(i)]) * dx(iN(i)));
            }
            (*Y)[std::size_t(k)][std::size_t(l)] = dI + cd(0, omega) * dQ;
        }
    }
    // Each contact's current's coefficients in δx: its held nodes' rows
    // (conduction, the carriers' own ∂/∂t, a Schottky contact's emission)
    // and jω its charge's - dI_c = g_c·δx.
    std::vector<Eigen::VectorXcd> gOf(static_cast<std::size_t>(nc));
    for (int k = 0; k < nc; ++k) {
        const Contact& c = a_contacts[std::size_t(k)];
        Eigen::VectorXcd w = Eigen::VectorXcd::Zero(3 * a_n), extra = Eigen::VectorXcd::Zero(3 * a_n);
        for (const auto& [i, area] : c.nodes) {
            if (a_heldKind[std::size_t(i)] != k) continue;
            w(iV(i)) += cd(0, omega);
            if (a_nodeMedium[std::size_t(i)] < 0) continue;
            if (c.kind == Contact::Schottky) {
                extra(iN(i)) += Q * area * a_C0 * c.vn;
                extra(iP(i)) -= Q * area * a_C0 * c.vp;
            } else {
                w(iP(i)) += Q * a_C0;
                w(iN(i)) -= Q * a_C0;
                extra(iP(i)) += cd(0, omega * a_boxP[std::size_t(i)]) * Q * a_C0;
                extra(iN(i)) -= cd(0, omega * a_boxN[std::size_t(i)]) * Q * a_C0;
            }
        }
        gOf[std::size_t(k)] = J.cast<cd>().transpose() * w + extra;
    }
    const auto currentsOf = [&](const Eigen::VectorXcd& dx) {
        std::vector<cd> out(static_cast<std::size_t>(nc));
        for (int k = 0; k < nc; ++k) out[std::size_t(k)] = gOf[std::size_t(k)].cwiseProduct(dx).sum();
        return out;
    };
    // The light modulated wholly (δG its optical generation's): each
    // contact's photocurrent per pair, H = δI/(q Gop).
    a_Hop.clear();
    if (!a_optical.empty() && a_Gop > 0) {
        Eigen::VectorXcd b = Eigen::VectorXcd::Zero(3 * a_n);
        for (int i = 0; i < a_n; ++i) {
            if (a_nodeMedium[std::size_t(i)] < 0) continue;
            const double g = a_generation[std::size_t(i)] - (a_genBase.empty() ? 0.0 : a_genBase[std::size_t(i)]);
            for (int r : {iN(i), iP(i)})
                if (!fixed[std::size_t(r)]) b(r) = a_box[std::size_t(i)] * g / a_C0;
        }
        const std::vector<cd> dI = currentsOf(lu.solve(b));
        for (int k = 0; k < nc; ++k) a_Hop.push_back(dI[std::size_t(k)]);
    }
    a_Y = *Y;
    a_noiseS.clear();
    if (!a_noise) {
        loads(omega);
        return true;
    }
    // Noise, by the impedance field: each microscopic source a Poisson
    // process - every edge's carriers' hops (diffusion's noise, Nyquist's
    // 4kT G at equilibrium), every node's pairs made and recombined, every
    // ionization - its current to each contact its Green's function's,
    // λ_c·b, λ_c from the transposed system (Aᵀ λ = g_c), and its own flux
    // into a contact's node directly: S_ab = Σ 2 (rate) h_a h_b* (2D's).
    Eigen::SparseMatrix<cd> At = A.transpose();
    At.makeCompressed();
    SparseLu<cd> lut;
    lut.compute(At);
    if (lut.info() != Eigen::Success) {
        *error = tr("%1: the noise's transposed small-signal matrix could not be factorized").arg(a_node->name());
        return false;
    }
    std::vector<Eigen::VectorXcd> lambda(static_cast<std::size_t>(nc));
    for (int k = 0; k < nc; ++k) lambda[std::size_t(k)] = lut.solve(gOf[std::size_t(k)]);
    a_noiseS.assign(std::size_t(nc), std::vector<cd>(std::size_t(nc), cd(0, 0)));
    std::vector<cd> h(static_cast<std::size_t>(nc));
    // (A source of unit rate on rows r1, r2 (b1, b2 its δx's right-hand
    // side), its direct currents into the contacts d.)
    const auto addSource = [&](double rate, int r1, double b1, int r2, double b2, const std::vector<double>& direct) {
        if (!(rate > 0)) return;
        for (int k = 0; k < nc; ++k) {
            cd v(0, 0);
            if (r1 >= 0 && !fixed[std::size_t(r1)]) v += lambda[std::size_t(k)](r1) * b1;
            if (r2 >= 0 && !fixed[std::size_t(r2)]) v += lambda[std::size_t(k)](r2) * b2;
            if (!direct.empty()) v += direct[std::size_t(k)];
            h[std::size_t(k)] = v;
        }
        for (int a = 0; a < nc; ++a)
            for (int b = 0; b < nc; ++b) a_noiseS[std::size_t(a)][std::size_t(b)] += 2 * rate * h[std::size_t(a)] * std::conj(h[std::size_t(b)]);
    };
    // Each edge's diffusion noise, each carrier: an extra flux δ along it
    // (F(i) += δ/C0, F(j) -= δ/C0) of 4 D n per volume over its dual face -
    // its constant current's density profile's harmonic mean n_h, Nyquist's
    // at equilibrium: rate 2 w Vt n_h. (Not where it ionizes: there the
    // noise is its ionizations' - McIntyre's.)
    const auto harmonic = [](double ni, double nj, double d) {
        ni = std::max(ni, 1e-300);
        nj = std::max(nj, 1e-300);
        const double r = nj / ni * std::exp(-d), lr = std::log(r);
        const double phi = std::abs(r - 1) > 1e-8 ? lr / (r - 1) : 1 - 0.5 * (r - 1);
        const double spread = std::abs(d) > 1e-8 ? -std::expm1(-d) / d : 1 - 0.5 * d;
        return ni / (spread * phi);
    };
    std::vector<char> ionizes(a_edges.size(), 0);
    if (a_ionRate.size() == a_prisms.size()) {
        const double most = *std::max_element(a_ionRate.begin(), a_ionRate.end());
        if (most > 0)
            for (std::size_t e = 0; e < a_prisms.size(); ++e)
                if (a_ionRate[e] > 1e-6 * most)
                    for (int k = 0; k < a_prisms[e].edgeCount; ++k) ionizes[std::size_t(a_prisms[e].edges[std::size_t(k)])] = 1;
    }
    const double Vt = a_Vt;
    std::vector<double> lgN, lgP;
    degeneracy(x, lgN, lgP);
    for (std::size_t mi = 0; mi < a_edges.size(); ++mi) {
        const Edge& e = a_edges[mi];
        if (e.wn <= 0 || ionizes[mi] || a_nodeMedium[std::size_t(e.i)] < 0 || a_nodeMedium[std::size_t(e.j)] < 0) continue;
        const int i = e.i, j = e.j;
        const EdgeState st = edgeState(e, x, lgN, lgP);
        const double Dn = st.Dn, Dp = st.Dp;
        for (int carrier = 0; carrier < 2; ++carrier) {
            const bool electrons = carrier == 0;
            const double w = electrons ? e.wn : e.wp;
            const double ci = (electrons ? st.ni : st.pi) * a_C0, cj = (electrons ? st.nj : st.pj) * a_C0;
            const double rate = 2 * w * Vt * harmonic(ci, cj, electrons ? Dn : -Dp);
            std::vector<double> direct(std::size_t(nc), 0.0);
            // (Its flux into an ohmic contact's node: that contact's current
            // at once - electrons' against it.)
            for (const auto& [node, sign] : {std::pair{i, 1.0}, std::pair{j, -1.0}}) {
                const int ck = a_heldKind[std::size_t(node)];
                if (ck < 0 || a_contacts[std::size_t(ck)].kind != Contact::Ohmic) continue;
                direct[std::size_t(ck)] += (electrons ? -Q : Q) * sign;
            }
            const int r1 = electrons ? iN(i) : iP(i), r2 = electrons ? iN(j) : iP(j);
            addSource(rate, r1, -1 / a_C0, r2, 1 / a_C0, direct);
        }
    }
    // Each node's pairs, made or recombined (each a pair at it): the light's
    // and the others' made, the thermal ones both ways - SRH's (n p +
    // ni²)/D, Auger's, direct's - and the ionizations at its prisms' corners.
    std::vector<double> events(std::size_t(a_n), 0.0);
    const Mesh3& mesh = a_s.mesh();
    for (int i = 0; i < a_n; ++i) {
        const int d = a_nodeMedium[std::size_t(i)];
        if (d < 0) continue;
        const Medium& m = a_media[std::size_t(d)];
        const double box = a_box[std::size_t(i)];
        events[std::size_t(i)] += std::abs(a_generation[std::size_t(i)]) * box;
        const double n = x(iN(i)) * a_C0, p = x(iP(i)) * a_C0;
        const double ni2 = std::exp(a_an[std::size_t(i)] + a_ap[std::size_t(i)]), ni = std::sqrt(ni2), both = n * p + ni2;
        double rate = 0;
        if (m.srh) {
            const double kT = KB * a_T;
            const double n1 = ni * std::exp(m.Et / kT), p1 = ni * std::exp(-m.Et / kT);
            double tn = a_tauN[std::size_t(i)], tp = a_tauP[std::size_t(i)];
            if (m.tat && a_field[std::size_t(i)] > 0) {
                double gn = 0, gp = 0, dg = 0;
                const double gap = a_gap[std::size_t(i)], ecEi = m.dEc - gap / 2, eiEv = m.Eg - gap - ecEi;
                detail::hurkx(a_field[std::size_t(i)], m.FGn, (ecEi - m.Et) / kT, &gn, &dg);
                detail::hurkx(a_field[std::size_t(i)], m.FGp, (eiEv + m.Et) / kT, &gp, &dg);
                tn /= 1 + gn;
                tp /= 1 + gp;
            }
            rate += both / (tp * (n + n1) + tn * (p + p1));
        }
        rate += (m.Cn * n + m.Cp * p) * both + m.B * both;
        events[std::size_t(i)] += a_recBox[std::size_t(i)] * rate;
    }
    if (a_ionRate.size() == a_prisms.size())
        for (std::size_t e = 0; e < a_prisms.size(); ++e) {
            if (!(a_ionRate[e] > 0)) continue;
            for (int k = 0; k < 6; ++k) events[std::size_t(mesh.prisms[e][std::size_t(k)])] += a_ionRate[e] * a_prisms[e].volume / 6;
        }
    const std::vector<double> none;
    for (int i = 0; i < a_n; ++i)
        if (events[std::size_t(i)] > 0) addSource(events[std::size_t(i)], iN(i), 1 / a_C0, iP(i), 1 / a_C0, none);
    loads(omega);
    return true;
}

void SemiconductorProblem3::loads(double omega)
{
    // Behind circuits: the contacts' voltages v from (Y + jω Cp + 1/Z) v =
    // -h on those contacts, the others held; the current through Z into the
    // device -v/Z - the light's through the load; the noise the same way,
    // I_L = (1/Z) M⁻¹ i, its powers M⁻¹ C M⁻ᴴ/Z² (the circuit's own
    // resistors' not in it) - 2D's.
    using cd = std::complex<double>;
    a_Hc.clear();
    a_noiseL.clear();
    const std::size_t nc = a_contacts.size();
    std::vector<std::size_t> ckt;
    for (std::size_t k = 0; k < nc; ++k)
        if (a_contacts[k].iC >= 0 && a_contacts[k].Rs + a_contacts[k].RL > 0) ckt.push_back(k);
    if (ckt.empty() || a_Y.size() != nc) return;
    const Eigen::Index m = Eigen::Index(ckt.size());
    Eigen::MatrixXcd M(m, m);
    for (Eigen::Index a = 0; a < m; ++a) {
        const Contact& c = a_contacts[ckt[std::size_t(a)]];
        for (Eigen::Index e = 0; e < m; ++e) M(a, e) = a_Y[ckt[std::size_t(a)]][ckt[std::size_t(e)]];
        M(a, a) += cd(0, omega * c.Cp) + 1.0 / (c.Rs + c.RL);
    }
    if (a_Hop.size() == nc) {
        a_Hc = a_Hop;
        Eigen::VectorXcd b(m);
        for (Eigen::Index a = 0; a < m; ++a) b(a) = -a_Hop[ckt[std::size_t(a)]];
        const Eigen::VectorXcd v = M.fullPivLu().solve(b);
        for (Eigen::Index a = 0; a < m; ++a) a_Hc[ckt[std::size_t(a)]] = -v(a) / (a_contacts[ckt[std::size_t(a)]].Rs + a_contacts[ckt[std::size_t(a)]].RL);
    }
    if (a_noiseS.size() == nc) {
        Eigen::MatrixXcd C(m, m);
        for (Eigen::Index a = 0; a < m; ++a)
            for (Eigen::Index e = 0; e < m; ++e) C(a, e) = a_noiseS[ckt[std::size_t(a)]][ckt[std::size_t(e)]];
        const Eigen::MatrixXcd Mi = M.fullPivLu().inverse();
        const Eigen::MatrixXcd S = Mi * C * Mi.adjoint();
        a_noiseL.assign(nc, NaN);
        for (Eigen::Index a = 0; a < m; ++a) {
            const Contact& c = a_contacts[ckt[std::size_t(a)]];
            a_noiseL[ckt[std::size_t(a)]] = S(a, a).real() / ((c.Rs + c.RL) * (c.Rs + c.RL));
        }
    }
}

bool SemiconductorProblem3::opticalGeneration(QString* error)
{
    // Each node's pairs: of each source over its domains, η P0 ε0 Im εr
    // |E|²/ħ - the power the light loses there, ω ε0 Im εr |E|² (the wave's
    // fields of 1 W fed as its modes are normalized, ∫ E×H·dA = 1: no ½),
    // over ħω/η a pair -, at the node's own |E|, its media's εr their mean over
    // its box. (Without a time of its own the base generation's; with P0 of
    // t, at each step.)
    if (a_genBase.empty()) {
        a_genBase = a_generation;
        a_genPhotonBase = a_genPhoton;
        a_genGapBase = a_genGapRate;
    }
    std::vector<double> add(std::size_t(a_n), 0.0), energy(std::size_t(a_n), 0.0);
    const Mesh3& mesh = a_s.mesh();
    const std::vector<const Node*> mats = materials();
    a_Gop = a_Pabs = a_Pin = 0;
    for (const Optical& o : a_optical) {
        int fi = -1;
        for (int k = 0; k < int(a_s.fields().size()); ++k)
            if (a_s.fields()[std::size_t(k)].tag == o.wave && a_s.fields()[std::size_t(k)].variable == QLatin1String("normE") && a_s.fields()[std::size_t(k)].solved) fi = k;
        const std::optional<double> freq = a_s.global(o.wave + QStringLiteral(".freq"));
        if (fi < 0 || !freq || !(*freq > 0)) {
            *error = tr("%1: %2 has no light from %3 - a Wavelength Domain step solving it comes before this one").arg(a_node->name(), o.name, o.wave);
            return false;
        }
        const Field3& wf = a_s.fields()[std::size_t(fi)];
        double P0 = 0;
        {
            std::vector<double> vals(std::size_t(a_s.pointScope().slotCount()), NaN);
            for (int slot : o.P0.variables())
                if (a_s.definitions()[std::size_t(slot)].kind == Solution3::VariableDef::Time) vals[std::size_t(slot)] = a_s.time();
            P0 = o.P0.isConstant() ? o.P0.constant() : o.P0.isValid() ? o.P0.eval(vals.data()) : NaN;
        }
        if (!std::isfinite(P0) || P0 < 0) {
            *error = tr("%1: %2's power fed is not a number of at least 0").arg(a_node->name(), o.name);
            return false;
        }
        a_Pin += P0;
        const double omega = 2 * M_PI * *freq, lambda = 299792458.0 / *freq;
        const double photon = HbarSI * omega / o.eta;   // (each pair's energy as made)
        // Each domain's Im εr at the wavelength.
        std::vector<double> imEps(a_media.size(), 0.0);
        for (std::size_t d = 0; d < a_media.size(); ++d)
            if (o.on[d] && d < mats.size() && mats[d]) imEps[d] = std::max(0.0, -relativePermittivity(*mats[d], lambda, a_s.parameters().scope).imag());
        // (By node: its box's media over the source's domains.)
        std::vector<double> sum(std::size_t(a_n), 0.0), vol(std::size_t(a_n), 0.0);
        for (int e = 0; e < int(a_prisms.size()); ++e) {
            const Prism& pr = a_prisms[std::size_t(e)];
            if (pr.domain < 0 || !o.on[std::size_t(pr.domain)]) continue;
            for (int k = 0; k < 6; ++k) {
                const int i = mesh.prisms[std::size_t(e)][std::size_t(k)];
                sum[std::size_t(i)] += pr.volume * imEps[std::size_t(pr.domain)];
                vol[std::size_t(i)] += pr.volume;
            }
        }
        for (int i = 0; i < a_n; ++i) {
            if (!(vol[std::size_t(i)] > 0) || a_nodeMedium[std::size_t(i)] < 0 || std::size_t(i) >= wf.values.size()) continue;
            const double E = wf.values[std::size_t(i)];
            const double lost = omega * Eps0 * (sum[std::size_t(i)] / vol[std::size_t(i)]) * E * E * P0;   // W/m³
            const double G = lost / photon;
            add[std::size_t(i)] += G;
            energy[std::size_t(i)] += G * photon;
            a_Gop += G * a_box[std::size_t(i)];
            a_Pabs += lost * a_box[std::size_t(i)];
        }
    }
    for (int i = 0; i < a_n; ++i) {
        a_generation[std::size_t(i)] = a_genBase[std::size_t(i)] + add[std::size_t(i)];
        a_genPhoton[std::size_t(i)] = a_genPhotonBase[std::size_t(i)] + energy[std::size_t(i)];
        a_genGapRate[std::size_t(i)] = a_genGapBase[std::size_t(i)];
    }
    return true;
}

void SemiconductorProblem3::ionizationIntegrals(const QString& p)
{
    // The ionization integrals along the field's lines through where it
    // ionizes a thousandth of its most or more, the most ionizing first, a
    // line from each start whose prism no line has crossed yet (2000 at
    // most), α of |E|: In = ∫ αn e^(-∫_x^W (αn - αp)) dx, Ip = ∫ αp
    // e^(-∫_0^x (αp - αn)) dx, x along the field - the most of each kept
    // (2D's). The field recovered at the nodes, each domain's own (its
    // prisms' by volume), linear within a prism.
    const Mesh3& mesh = a_s.mesh();
    const Space3& sp = *field().space;
    const Eigen::VectorXd& x = a_x;
    const auto ionizes = [&](int e) { return e >= 0 && e < int(a_prisms.size()) && a_prisms[std::size_t(e)].domain >= 0 && a_media[std::size_t(a_prisms[std::size_t(e)].domain)].ionization; };
    std::map<std::pair<int, int>, std::array<double, 4>> recovered;   // (node, domain): Σ vol E, Σ vol
    for (int e = 0; e < int(a_prisms.size()); ++e) {
        if (!ionizes(e)) continue;
        double E[3];
        fieldIn(e, x, E);
        const Prism& pr = a_prisms[std::size_t(e)];
        for (int k = 0; k < 6; ++k) {
            auto& r = recovered[{mesh.prisms[std::size_t(e)][std::size_t(k)], pr.domain}];
            for (int c = 0; c < 3; ++c) r[std::size_t(c)] += pr.volume * E[c];
            r[3] += pr.volume;
        }
    }
    const auto cornerField = [&](int e, int k, double E[3]) {
        const auto it = recovered.find({mesh.prisms[std::size_t(e)][std::size_t(k)], a_prisms[std::size_t(e)].domain});
        for (int c = 0; c < 3; ++c) E[c] = it != recovered.end() && it->second[3] > 0 ? it->second[std::size_t(c)] / it->second[3] : 0.0;
    };
    const auto fieldAt = [&](int e, double xi, double eta, double zeta, double E[3]) {
        const double l = 1 - xi - eta;
        const double w[6] = {l * (1 - zeta), xi * (1 - zeta), eta * (1 - zeta), l * zeta, xi * zeta, eta * zeta};
        E[0] = E[1] = E[2] = 0;
        for (int k = 0; k < 6; ++k) {
            double c[3];
            cornerField(e, k, c);
            for (int a = 0; a < 3; ++a) E[a] += std::clamp(w[k], 0.0, 1.0) * c[a];
        }
    };
    const auto alphas = [&](int e, double E, double* an, double* ap) {
        const Prism& pr = a_prisms[std::size_t(e)];
        const Medium& m = a_media[std::size_t(pr.domain)];
        double TL = 0;
        for (int k = 0; k < 6; ++k) TL += a_TL[std::size_t(mesh.prisms[std::size_t(e)][std::size_t(k)])] / 6;
        const double g = detail::phononGamma(m.ionPhonon, TL);
        double d = 0;
        detail::ionizationRate(E, g * m.an, g * m.bn, g * m.an2, g * m.bn2, m.E0, m.ionBetaN, an, &d);
        detail::ionizationRate(E, g * m.ap, g * m.bp, g * m.ap2, g * m.bp2, m.E0, m.ionBetaP, ap, &d);
    };
    struct Seed {
        double alpha, E;
        int e, k;
    };
    std::vector<Seed> seeds;
    double Emax = 0, most = 0;
    for (int e = 0; e < int(a_prisms.size()); ++e) {
        if (!ionizes(e)) continue;
        for (int k = 0; k < 6; ++k) {
            double E[3];
            cornerField(e, k, E);
            const double v = std::sqrt(E[0] * E[0] + E[1] * E[1] + E[2] * E[2]);
            Emax = std::max(Emax, v);
            if (!(v > 0)) continue;
            double an = 0, ap = 0;
            alphas(e, v, &an, &ap);
            most = std::max(most, std::max(an, ap));
            seeds.push_back({std::max(an, ap), v, e, k});
        }
    }
    double In = NaN, Ip = NaN, length = NaN, at[3] = {NaN, NaN, NaN};
    int lines = 0;
    if (!seeds.empty() && Emax > 0) {
        seeds.erase(std::remove_if(seeds.begin(), seeds.end(), [most](const Seed& sd) { return !(sd.alpha >= 1e-3 * most) || !(sd.alpha > 0); }), seeds.end());
        std::sort(seeds.begin(), seeds.end(), [](const Seed& a, const Seed& b) { return a.alpha > b.alpha; });
        In = Ip = 0;
        std::vector<char> covered(a_prisms.size(), 0);
        PointEvaluator3 ev(a_s);
        struct Sample {
            double s, E;
            int e;
            Vec3 p;
        };
        const auto sizeOf = [&](int e) {
            const int j = mesh.prismLayer[std::size_t(e)];
            const double h = (mesh.heights[std::size_t(j) + 1] - mesh.heights[std::size_t(j)]) * mesh.unitScale;
            return std::min(h, std::sqrt(2 * a_prisms[std::size_t(e)].volume / h));
        };
        // Along the field (sign 1) or against it: each step a fifth of its
        // prism's size, until it leaves what ionizes, the field turns back
        // or falls to a thousandth of the highest.
        const auto follow = [&](int start, Vec3 from, double sign) {
            std::vector<Sample> out;
            int e = start;
            Vec3 pnt = from;
            double sAlong = 0;
            if (!ev.at(pnt)) return out;
            for (int step = 0; step < 200000; ++step) {
                double E[3];
                fieldAt(ev.prism() == e ? e : e, ev.xi(), ev.eta(), ev.zeta(), E);
                const double v = std::sqrt(E[0] * E[0] + E[1] * E[1] + E[2] * E[2]);
                if (!(v > 1e-3 * Emax)) break;
                const double h = 0.2 * sizeOf(e);
                const Vec3 d{sign * E[0] / v, sign * E[1] / v, sign * E[2] / v};
                const Vec3 q{pnt.x + d.x * h, pnt.y + d.y * h, pnt.z + d.z * h};
                if (!ev.at(q) || !ionizes(ev.prism())) break;
                const int next = ev.prism();
                double En[3];
                fieldAt(next, ev.xi(), ev.eta(), ev.zeta(), En);
                if ((En[0] * d.x + En[1] * d.y + En[2] * d.z) * sign <= 0) break;   // (the field turned)
                sAlong += h;
                out.push_back({sAlong, std::sqrt(En[0] * En[0] + En[1] * En[1] + En[2] * En[2]), next, q});
                covered[std::size_t(next)] = 1;
                pnt = q;
                e = next;
            }
            return out;
        };
        for (const Seed& sd : seeds) {
            if (covered[std::size_t(sd.e)] && lines > 0) continue;
            if (lines >= 2000) break;
            const auto& cn = mesh.prisms[std::size_t(sd.e)];
            Vec3 mid{0, 0, 0};
            for (int k = 0; k < 6; ++k) {
                const Vec3 q = sp.point(cn[std::size_t(k)]);
                mid = Vec3{mid.x + q.x / 6, mid.y + q.y / 6, mid.z + q.z / 6};
            }
            const Vec3 c = sp.point(cn[std::size_t(sd.k)]);
            const Vec3 from{0.98 * c.x + 0.02 * mid.x, 0.98 * c.y + 0.02 * mid.y, 0.98 * c.z + 0.02 * mid.z};
            covered[std::size_t(sd.e)] = 1;
            const std::vector<Sample> against = follow(sd.e, from, -1), along = follow(sd.e, from, 1);
            std::vector<Sample> line;
            for (auto it = against.rbegin(); it != against.rend(); ++it) line.push_back({-it->s, it->E, it->e, it->p});
            line.push_back({0, sd.E, sd.e, from});
            for (const Sample& sm : along) line.push_back(sm);
            ++lines;
            if (line.size() < 2) continue;
            const std::size_t n = line.size();
            std::vector<double> xs(n), an(n), ap(n), K(n, 0.0);
            for (std::size_t k = 0; k < n; ++k) {
                xs[k] = line[k].s - line.front().s;
                alphas(line[k].e, line[k].E, &an[k], &ap[k]);
            }
            for (std::size_t k = 1; k < n; ++k) K[k] = K[k - 1] + 0.5 * ((ap[k] - an[k]) + (ap[k - 1] - an[k - 1])) * (xs[k] - xs[k - 1]);
            double in = 0, ip = 0;
            for (std::size_t k = 1; k < n; ++k) {
                const double dx = xs[k] - xs[k - 1];
                ip += 0.5 * (ap[k] * std::exp(-K[k]) + ap[k - 1] * std::exp(-K[k - 1])) * dx;
                in += 0.5 * (an[k] * std::exp(K[n - 1] - K[k]) + an[k - 1] * std::exp(K[n - 1] - K[k - 1])) * dx;
            }
            if (in > In || !std::isfinite(length)) {
                In = in;
                length = xs.back();
                const Sample* top = &line.front();
                for (const Sample& sm : line)
                    if (sm.E > top->E) top = &sm;
                at[0] = top->p.x;
                at[1] = top->p.y;
                at[2] = top->p.z;
            }
            Ip = std::max(Ip, ip);
        }
    }
    const Dim fieldDim = Dim::of(1, 1, -3, -1);
    a_s.setGlobal(p + QStringLiteral("In"), In, Dim(),
                  tr("electrons' ionization integral, the most of the field lines' through where it ionizes a thousandth of its most or more (%1 lines), "
                     "α of the field |E|: breakdown where it reaches 1")
                      .arg(lines));
    a_s.setGlobal(p + QStringLiteral("Ip"), Ip, Dim(), tr("holes' ionization integral, the most of those lines', α of |E|: breakdown where it reaches 1"));
    a_s.setGlobal(p + QStringLiteral("Mn"), In < 1 ? 1 / (1 - In) : Inf, Dim(), tr("multiplication of electrons entering In's line at its end, 1/(1 - In)"));
    a_s.setGlobal(p + QStringLiteral("Mp"), Ip < 1 ? 1 / (1 - Ip) : Inf, Dim(), tr("multiplication of holes entering Ip's line at its start, 1/(1 - Ip)"));
    a_s.setGlobal(p + QStringLiteral("Emax"), Emax > 0 ? Emax : NaN, fieldDim, tr("the highest field where it ionizes"));
    a_s.setGlobal(p + QStringLiteral("Lion"), length, Dim::length(), tr("In's field line's length"));
    a_s.setGlobal(p + QStringLiteral("xion"), at[0], Dim::length(), tr("x of In's field line's highest field: where it breaks down first"));
    a_s.setGlobal(p + QStringLiteral("yion"), at[1], Dim::length(), tr("y of In's field line's highest field"));
    a_s.setGlobal(p + QStringLiteral("zion"), at[2], Dim::length(), tr("z of In's field line's highest field"));
}

void SemiconductorProblem3::setGlobals()
{
    const QString p = field().tag + QLatin1Char('.');
    if (!a_optical.empty() && field().solved) {
        a_s.setGlobal(p + QStringLiteral("Gop"), a_Gop, Dim::of(0, 0, -1), tr("optical generation's pairs a second: the light's photons absorbed in its domains, times η"));
        a_s.setGlobal(p + QStringLiteral("Iop"), Q * a_Gop, Dim::current(), tr("optical generation's current, q Gop"));
        a_s.setGlobal(p + QStringLiteral("Pabs"), a_Pabs, Dim::power(), tr("the light's power absorbed in its domains (W)"));
    }
    // Noise at the frequency: each contact's short-circuit noise current,
    // its ratio to its current's full shot noise 2q|I|; lit, each contact's
    // photocurrent per pair.
    if (a_noiseS.size() == a_contacts.size())
        for (std::size_t c = 0; c < a_contacts.size(); ++c) {
            const QString& name = a_contacts[c].name;
            const double S = a_noiseS[c][c].real();
            a_s.setGlobal(p + QStringLiteral("Sn_") + name, S, Dim::of(0, 0, 1, 2),
                          tr("noise current spectral density at %1, short-circuited (A²/Hz, one-sided): the carriers' diffusion, their pairs made "
                             "and recombined, their ionizations - by the impedance field")
                              .arg(name));
            a_s.setGlobal(p + QStringLiteral("in_") + name, std::sqrt(std::max(S, 0.0)), Dim(), tr("noise current at %1, A/√Hz").arg(name));
            const double I = std::abs(a_contacts[c].current);
            a_s.setGlobal(p + QStringLiteral("Fshot_") + name, I > 0 ? S / (2 * Q * I) : NaN, Dim(),
                          tr("its noise over its current's full shot noise, 2q|I|: 1 a photodiode's").arg(name));
        }
    if (!a_optical.empty()) a_s.setGlobal(p + QStringLiteral("Pin"), a_Pin, Dim::power(), tr("the light fed (W) - a responsivity |I|/Pin"));
    if (a_Hop.size() == a_contacts.size() && a_Gop > 0) {
        const double RL = a_optical.empty() ? 50.0 : a_optical.front().RL;
        for (std::size_t c = 0; c < a_contacts.size(); ++c) {
            const QString& name = a_contacts[c].name;
            const std::complex<double> h = a_Hop[c] / (Q * a_Gop);
            a_s.setGlobal(p + QStringLiteral("Hop_") + name, std::abs(h), Dim(),
                          tr("small-signal photocurrent at %1 per pair made, the light modulated (1 at low frequency, every pair collected): its "
                             "transit's roll-off")
                              .arg(name));
            if (a_Hc.size() > c && a_contacts[c].iC >= 0)
                a_s.setGlobal(p + QStringLiteral("Hc_") + name, std::abs(a_Hc[c] / (Q * a_Gop)), Dim(),
                              tr("the small-signal photocurrent through %1's circuit's load per pair made: its RC and the device's, Cp across").arg(name));
            if (RL > 0 && a_Y.size() > c) {
                const std::complex<double> yl = 1 / RL, loaded = h * yl / (yl + a_Y[c][c]);
                a_s.setGlobal(p + QStringLiteral("HopRL_") + name, std::abs(loaded), Dim(),
                              tr("the same through the load RL across %1 (the device's capacitance charged through it): a photodiode's bandwidth, "
                                 "where it is 1/√2 of its low frequency's")
                                  .arg(name));
            }
        }
    }
    if (a_noiseL.size() == a_contacts.size() || (a_noiseS.size() == a_contacts.size() && a_Pin > 0 && a_Hop.size() == a_contacts.size()))
        for (std::size_t c = 0; c < a_contacts.size(); ++c) {
            const QString& name = a_contacts[c].name;
            if (a_noiseL.size() > c && std::isfinite(a_noiseL[c]))
                a_s.setGlobal(p + QStringLiteral("SnL_") + name, a_noiseL[c], Dim::of(0, 0, 1, 2),
                              tr("the device's noise current through %1's circuit's load (A²/Hz; the circuit's own resistors' not in it)").arg(name));
            if (a_noiseS.size() > c && a_Pin > 0 && a_Hop.size() > c && std::abs(a_Hop[c]) > 0)
                a_s.setGlobal(p + QStringLiteral("NEP_") + name, std::sqrt(std::max(a_noiseS[c][c].real(), 0.0)) / (std::abs(a_Hop[c]) / a_Pin), Dim(),
                              tr("noise-equivalent power at %1, W/√Hz: its noise current over its responsivity at the frequency").arg(name));
        }
    // Stimulated recombination's current, q ∫ R_stim.
    if (!a_stimulated.empty() && field().solved) {
        double current = 0;
        if (a_stimRate.size() == std::size_t(a_n))
            for (int i = 0; i < a_n; ++i) current += Q * a_box[std::size_t(i)] * a_stimRate[std::size_t(i)];
        a_s.setGlobal(p + QStringLiteral("Istim"), current, Dim::current(),
                      tr("stimulated recombination's current, q ∫ c (⟨R⟩ - R0) over its domains: its photons' rate times q"));
    }
    // Impact ionization: its integrals; its pairs against those made
    // otherwise.
    bool ionizing = false, inEquations = false;
    for (const Medium& m : a_media) {
        ionizing = ionizing || (m.semiconductor && m.ionization);
        inEquations = inEquations || (m.semiconductor && m.ionization && m.ionCurrents);
    }
    if (ionizing && field().solved && a_solvedOnce) {
        ionizationIntegrals(p);
        if (inEquations) {
            const double Iion = Q * a_ionPairs, Igen = Q * a_genPairs;
            double most = 0;
            bool still = true;
            for (const Contact& c : a_contacts) {
                still = still && c.current == 0;
                most = std::max(most, std::abs(c.current));
            }
            a_s.setGlobal(p + QStringLiteral("Iion"), Iion, Dim::current(), tr("current of impact ionization's pairs, q ∫ G: at breakdown the whole current"));
            a_s.setGlobal(p + QStringLiteral("Igen"), Igen, Dim::current(),
                          tr("current of the pairs made otherwise, q ∫ G of their generation: thermal, tunnelling, light, surfaces"));
            a_s.setGlobal(p + QStringLiteral("Mion"), still ? 1.0 : Igen > 1e-12 * most ? 1 + Iion / Igen : NaN, Dim(),
                          tr("impact ionization's multiplication of the pairs made otherwise, 1 + Iion/Igen"));
        }
    }
    // Fowler-Nordheim tunnelling's current, after the solve: at each face
    // point, the barrier's mean field - φB over the depth into the insulator
    // at which the potential has moved by φB/q, along its normal (or its
    // change over a thinner insulator: tunnelling direct).
    if (!a_fn.empty() && field().solved) {
        const Mesh3& mesh = a_s.mesh();
        PointEvaluator3 ev(a_s);
        const Expression V = Expression::compile(p + QStringLiteral("V"), a_s.pointScope());
        const auto potential = [&](const Vec3& y, int domain, double* out) {
            if (!ev.at(y) || ev.domain() != domain) return false;
            *out = ev.eval(V);
            return std::isfinite(*out);
        };
        // (A step a tenth of the prism there: its layer's height, its
        // triangle's size, the less.)
        const auto stepAt = [&]() {
            const int e = ev.prism();
            if (e < 0) return 0.0;
            const int j = mesh.prismLayer[std::size_t(e)];
            const double h = (mesh.heights[std::size_t(j) + 1] - mesh.heights[std::size_t(j)]) * mesh.unitScale;
            const auto& pr = mesh.prisms[std::size_t(e)];
            const Vec3 a = mesh.nodes[std::size_t(pr[0])], b = mesh.nodes[std::size_t(pr[1])], c = mesh.nodes[std::size_t(pr[2])];
            const double area = std::abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) / 2 * mesh.unitScale * mesh.unitScale;
            return std::min(h, std::sqrt(2 * area)) / 10;
        };
        double current = 0;
        for (const FowlerNordheim& fn : a_fn)
            for (const FowlerNordheim::Point& pt : fn.points) {
                double V0 = 0;
                if (!ev.at(pt.x)) continue;
                V0 = ev.eval(V);
                // Into the insulator.
                const double tiny = 1e-6 * std::sqrt(pt.area);
                Vec3 d = pt.n;
                double probe = 0;
                if (!potential(Vec3{pt.x.x + d.x * tiny, pt.x.y + d.y * tiny, pt.x.z + d.z * tiny}, pt.insulator, &probe)) {
                    d = Vec3{-d.x, -d.y, -d.z};
                    if (!potential(Vec3{pt.x.x + d.x * tiny, pt.x.y + d.y * tiny, pt.x.z + d.z * tiny}, pt.insulator, &probe)) continue;
                }
                double s = tiny, moved = probe - V0, E = 0;
                bool found = false;
                for (int hop = 0; hop < 100000; ++hop) {
                    const double step = stepAt();
                    if (!(step > 0)) break;
                    double next = 0;
                    const Vec3 y{pt.x.x + d.x * (s + step), pt.x.y + d.y * (s + step), pt.x.z + d.z * (s + step)};
                    if (!potential(y, pt.insulator, &next)) {
                        // Out of it within the step: its edge by halving, the
                        // potential's change there.
                        double lo = 0, hi = step;
                        for (int k = 0; k < 40; ++k) {
                            const double mid = 0.5 * (lo + hi);
                            double vm = 0;
                            if (potential(Vec3{pt.x.x + d.x * (s + mid), pt.x.y + d.y * (s + mid), pt.x.z + d.z * (s + mid)}, pt.insulator, &vm)) {
                                lo = mid;
                                next = vm;
                            } else
                                hi = mid;
                        }
                        if (lo > 0) {
                            const double change = next - V0;
                            if (std::abs(change) >= fn.phi) {
                                const double a = std::abs(moved), b = std::abs(change);
                                E = fn.phi / (s + lo * (fn.phi - a) / (b - a));
                                found = true;
                                break;
                            }
                            moved = change;
                            s += lo;
                        }
                        break;
                    }
                    const double change = next - V0;
                    if (std::abs(change) >= fn.phi) {
                        const double a = std::abs(moved), b = std::abs(change);
                        E = fn.phi / (s + step * (fn.phi - a) / (b - a));
                        found = true;
                        break;
                    }
                    moved = change;
                    s += step;
                    ev.at(y);
                }
                if (!found) E = s > 0 ? std::abs(moved) / s : 0.0;
                if (E > 0) current += fn.A * E * E * std::exp(-fn.B / E) * pt.area;
            }
        a_s.setGlobal(p + QStringLiteral("Ifn"), current, Dim::current(),
                      tr("Fowler-Nordheim tunnelling's current through the insulators asked, at the mean field over each one's barrier (after the solve)"));
    }
    for (const Contact& c : a_contacts) {
        a_s.setGlobal(p + QStringLiteral("I_") + c.name, c.current, Dim::current(), tr("current into the device at %1").arg(c.name));
        a_s.setGlobal(p + QStringLiteral("V_") + c.name, c.applied, Dim::voltage(), tr("contact %1's voltage").arg(c.name));
        a_s.setGlobal(p + QStringLiteral("Q_") + c.name, c.charge, Dim::charge(), tr("the charge on contact %1").arg(c.name));
        if (c.iC < 0) continue;
        // Behind a circuit: the contact's own voltage, and its load's
        // current, voltage and power (a bias tee's at rest none).
        const double IL = c.loadCurrent(inTime());
        a_s.setGlobal(p + QStringLiteral("Vc_") + c.name, c.own, Dim::voltage(), tr("the contact %1's own voltage: its source's less its circuit's drop").arg(c.name));
        a_s.setGlobal(p + QStringLiteral("IL_") + c.name, IL, Dim::current(),
                      c.circuit == Contact::BiasTee ? tr("current through %1's load RL toward the device: in time the change from rest, through its bias tee").arg(c.name)
                                                    : tr("current through %1's load RL toward the device").arg(c.name));
        a_s.setGlobal(p + QStringLiteral("VL_") + c.name, c.RL * IL, Dim::voltage(), tr("voltage across %1's load RL, RL IL").arg(c.name));
        a_s.setGlobal(p + QStringLiteral("PL_") + c.name, c.RL * IL * IL, Dim::power(), tr("power into %1's load RL, RL IL²").arg(c.name));
    }
}

} // namespace

std::unique_ptr<Problem3> makeSemiconductorProblem3(Solution3& solution, int field)
{
    return std::make_unique<SemiconductorProblem3>(solution, field);
}

bool semiconductorSmallSignal3(Problem3& problem, double frequency, std::vector<std::vector<std::complex<double>>>* Y, QStringList* names, QString* error)
{
    auto* s = dynamic_cast<SemiconductorProblem3*>(&problem);
    if (!s) {
        *error = QStringLiteral("not a semiconductor");
        return false;
    }
    if (names) *names = s->contactNames();
    return s->smallSignal(frequency, Y, error);
}

} // namespace qucs_s::fem::detail3
