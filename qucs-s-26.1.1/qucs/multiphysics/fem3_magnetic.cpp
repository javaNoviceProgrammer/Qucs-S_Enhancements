/*
 * fem3_magnetic.cpp - magnetic fields in 3D: magnetostatics, the vector
 *                     potential A on a swept mesh's edge elements (Nédélec's
 *                     of order 1 or 2), ∇ × (H(B)) = J with B = ∇ × A, its
 *                     gauge ∇·A = 0 held weakly by a nodal Lagrange
 *                     multiplier of the same order; µr materials (a tensor)
 *                     and B-H curves by Newton's method, currents given or
 *                     read from a currents physics (coils), permanent
 *                     magnets, magnetic insulation, a potential held, a
 *                     surface current; the energy, the coils' flux linkage
 *                     and inductance matrix, the Lorentz force on each coil;
 *                     in the frequency domain eddy currents jωσA with an
 *                     electric potential on the conductors' nodes, single
 *                     conductors fed at a currents physics' terminal (a
 *                     current or a voltage held), the coils' impedance
 *                     matrix, the losses (resistive, µr'') as time averages
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_problem.h"

#include "fem3_ams.h"
#include "fem_edge.h"
#include "fem_problem.h"
#include "fem_tensor.h"

#include <Eigen/Dense>

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace qucs_s::fem::detail3 {

namespace {

constexpr double Mu0 = 1.25663706127e-6;
const double NaN = std::numeric_limits<double>::quiet_NaN();

/// A face's own condition: the last feature's.
struct FaceCondition {
    enum Kind { Free, Insulation, Potential };
    Kind kind = Free;
    int order = -1;
    Expression a0[3];   // a potential's
};

/// A coil: its current density the currents physics' J between its
/// terminal and the rest, scaled to N I0 through it. In the frequency
/// domain a single conductor's is not given but found: its eddy currents
/// in it, its terminal's potential U (a current held, or no net current) or
/// its current (a voltage held, or none) - turns of wire still carry the
/// currents physics' J, evenly, no eddy currents in them.
struct Coil {
    enum Role { Current, Voltage, Floating, Grounded };
    QString name;
    std::vector<char> on;        // by domain
    Expression current, turns;   // I0, N
    bool single = true;          // a single conductor (not turns)
    Role role = Current;         // a single conductor's in the frequency domain
    Expression voltage;          // V0 (role Voltage)
    QString ec;                  // the currents physics' tag
    QString terminal;            // its terminal whose current J carries (empty: its first)
    double perAmp = 0;           // N / I_ec, as last solved: its J at 1 A
    double scale = 0;            // N I0 / I_ec
    double flux = 0;             // its flux linkage as solved (Wb)
    double force[3] = {0, 0, 0}; // ∫ J × B over it (N)
    Expression wireSigma;        // turns': their wire's conductivity (invalid: no resistance)
    double fill = 1;             // turns': the wire's share of its section
};

/// The reference prism's nodal shape functions' gradients by a straight
/// triangle's map (as the edge elements take theirs: the gauge's gradients
/// then the edge space's own).
void nodalGradients(const PrismShape& s, const AffineMap& am, double h, double g[18][3])
{
    for (int k = 0; k < s.count; ++k) {
        g[k][0] = s.dxi[k] * am.grad[1][0] + s.deta[k] * am.grad[2][0];
        g[k][1] = s.dxi[k] * am.grad[1][1] + s.deta[k] * am.grad[2][1];
        g[k][2] = s.dzeta[k] / h;
    }
}

/// A prolongation by node (\a vn: rows each node's value, columns the
/// coarse nodes') by equation (\a eq by node: its equation, < 0 none): each
/// equation's row its first node's, its columns the coarse nodes' equations,
/// numbered anew in turn - several nodes one equation (a terminal's), their
/// columns summed.
Eigen::SparseMatrix<double> byEquation(const Eigen::SparseMatrix<double>& vn, const std::vector<int>& eq, int n)
{
    std::vector<int> first(std::size_t(n), -1), column(std::size_t(n), -1);
    for (int k = 0; k < int(eq.size()); ++k)
        if (eq[std::size_t(k)] >= 0 && first[std::size_t(eq[std::size_t(k)])] < 0) first[std::size_t(eq[std::size_t(k)])] = k;
    int nc = 0;
    std::vector<Eigen::Triplet<double>> t;
    for (int c = 0; c < vn.outerSize(); ++c) {
        if (c >= int(eq.size()) || eq[std::size_t(c)] < 0) continue;
        for (Eigen::SparseMatrix<double>::InnerIterator it(vn, c); it; ++it) {
            const int r = int(it.row());
            if (r >= int(eq.size()) || eq[std::size_t(r)] < 0 || first[std::size_t(eq[std::size_t(r)])] != r) continue;
            int& col = column[std::size_t(eq[std::size_t(c)])];
            if (col < 0) col = nc++;
            t.emplace_back(eq[std::size_t(r)], col, it.value());
        }
    }
    Eigen::SparseMatrix<double> m(n, nc);
    m.setFromTriplets(t.begin(), t.end());
    return m;
}

/// \a m's columns (by node) by equation (\a eq by node, < 0 none): the
/// columns of nodes of one equation summed (a terminal's: the gradient of
/// its nodes' functions' sum).
Eigen::SparseMatrix<double> columnsByEquation(const Eigen::SparseMatrix<double>& m, const std::vector<int>& eq, int n)
{
    std::vector<Eigen::Triplet<double>> t;
    for (int c = 0; c < m.outerSize(); ++c) {
        if (c >= int(eq.size()) || eq[std::size_t(c)] < 0) continue;
        for (Eigen::SparseMatrix<double>::InnerIterator it(m, c); it; ++it) t.emplace_back(int(it.row()), eq[std::size_t(c)], it.value());
    }
    Eigen::SparseMatrix<double> out(m.rows(), n);
    out.setFromTriplets(t.begin(), t.end());
    return out;
}

/// ν (A m / (V s)): the inverse of µ0 µr, a 3 x 3 tensor (row by row).
bool reluctivity(const double mur[9], double nu[9])
{
    Eigen::Matrix3d m;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m(i, j) = Mu0 * mur[3 * i + j];
    Eigen::FullPivLU<Eigen::Matrix3d> lu(m);
    if (!lu.isInvertible()) return false;
    const Eigen::Matrix3d inv = lu.inverse();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) nu[3 * i + j] = inv(i, j);
    return true;
}

/// A B-H curve's effective permeability for a sinusoidal B of amplitude \a
/// bhat: B̂ / (µ0 H1), H1 the fundamental of H(t), (2/π) ∫ H(B̂ sin θ) sin θ
/// dθ over [0, π] (Simpson's rule); its first slope's at nought.
double effectiveMur(const BHCurve& curve, double bhat)
{
    if (!(bhat > 0)) return 1 / (Mu0 * curve.nu(0));
    const int n = 200;
    double sum = 0;
    for (int k = 0; k <= n; ++k) {
        const double th = M_PI * k / n, w = (k == 0 || k == n) ? 1 : (k % 2 ? 4 : 2);
        sum += w * curve.H(bhat * std::sin(th)) * std::sin(th);
    }
    const double h1 = 2 / M_PI * sum * (M_PI / n) / 3;
    return h1 > 0 ? bhat / (Mu0 * h1) : 1 / (Mu0 * curve.nu(0));
}

/// A curve's fundamental H1(B̂) tabulated (B̂ from 0 to past its last point),
/// to read either way: µr of a B̂, and of an Ĥ (the B̂ whose H1 it is).
struct EffectiveTable {
    std::vector<double> b, h;   // rising
    explicit EffectiveTable(const BHCurve& curve)
    {
        const double top = 1.5 * curve.B().back() + 1;
        const int n = 400;
        for (int k = 0; k <= n; ++k) {
            const double bh = top * k / n;
            b.push_back(bh);
            h.push_back(k == 0 ? 0.0 : bh / (Mu0 * effectiveMur(curve, bh)));
        }
    }
    static double at(const std::vector<double>& x, const std::vector<double>& y, double v)
    {
        if (v <= x.front()) return y.front();
        if (v >= x.back()) return y.back() + (v - x.back()) * (y.back() - y[y.size() - 2]) / (x.back() - x[x.size() - 2]);
        const std::size_t k = std::size_t(std::upper_bound(x.begin(), x.end(), v) - x.begin());
        const double t = (v - x[k - 1]) / (x[k] - x[k - 1]);
        return y[k - 1] + t * (y[k] - y[k - 1]);
    }
    double murOfB(double bh, double mu0r) const { return bh > 0 ? bh / (Mu0 * at(b, h, bh)) : mu0r; }
    double murOfH(double hh, double mu0r) const
    {
        if (!(hh > 0)) return mu0r;
        const double bh = at(h, b, hh);
        return bh / (Mu0 * hh);
    }
};

class MagneticProblem3 : public Problem3
{
public:
    using Problem3::Problem3;
    bool setup(QString* error) override;
    bool solve(const Options3& o, QString* error) override;
    void setGlobals() override;
    void releaseFactors() override
    {
        a_lu.reset();
        a_factored = Eigen::SparseMatrix<double>();
        a_stepping.reset();
        a_steppingLu.reset();
    }
    /// In time where it conducts: eddy currents σ (∂A/∂t + ∇V) - its
    /// domains' σ, or a single conductor's coil (its currents physics' σ,
    /// read as its conductors are built); and where a coil of turns is,
    /// nothing conducting, stepped too - its voltage R I + dΦ/dt of Ȧ (K0
    /// alone, factorized once). A B-H curve's not: solved at rest at each
    /// time (in time it is refused), its coils' voltage R I alone, said.
    bool hasMass() const override
    {
        if (conductsAnywhere()) return true;
        return !a_coils.empty() && !a_nonlinear;
    }
    /// Nothing conducting the field has no state of its own: a step's start
    /// its solution at rest then (a current not nought at the start not
    /// switched on at the first step: a constant 0.2 A's coil said R I + L
    /// I/h there, 14.1 mV for R I's 4.2).
    bool solvedAtRestFirst() const override { return !conductsAnywhere(); }
    bool conductsAnywhere() const
    {
        for (std::size_t d = 0; d < a_sigma.size(); ++d)
            if (conducts(int(d))) return true;
        for (const Coil& c : a_coils)
            if (c.single) return true;
        return false;
    }
    void beginStepping() override;

private:
    using cd = std::complex<double>;
    struct Equations {
        std::vector<int> eqA, eqP;     // by edge dof / node dof: its equation, -1 none, -3 fixed
        std::vector<double> fixedA;    // by edge dof: its held value (a potential's)
        int nA = 0, nP = 0;
    };
    bool buildEquations(Equations& q, QString* error);
    /// The frequency domain's conductors: the electric potential V on the
    /// nodes of the elements that conduct, its equations after A's and the
    /// gauge's - each single conductor's terminal's nodes one equation, its
    /// potential U (the coil's voltage), or held at its voltage; a ground's
    /// (any other face the currents physics holds) nought; one node of a
    /// conductor that touches neither held, its constant -, scaled by
    /// 1/(jω) as ψ = V/(jω) (the system complex symmetric).
    struct Conductors {
        std::vector<char> conducting;   // by domain
        std::vector<int> eqV;           // by node dof: its equation (offset by nA + nP), -1 none, -3 held
        std::vector<cd> heldV;          // by node dof: its held potential (V; ψ = V/(jω))
        std::vector<char> electrode;    // by node dof: a terminal's or a ground's
        std::vector<char> electrodeFaces;   // by mesh face: a terminal's or a ground's
        std::vector<int> terminalOf;    // by node dof: the coil whose terminal it is on, -1
        std::vector<int> eqU;           // by coil: its potential's equation (a current held), -1 none (turns, or its voltage held)
        int nV = 0;
    };
    bool buildConductors(Conductors& h, QString* error);
    /// The faces currents physics \a tag holds, by topology face - its
    /// terminal's index in \a names (a Terminal's name, a Floating's own), -2
    /// a ground's (Ground, Electric Potential), -1 none: the last condition
    /// on a face its own, as that physics takes them.
    std::vector<int> heldFacesOf(const QString& tag, QStringList* names) const;
    /// What coil \a c's currents physics' J carries in at its terminal in
    /// this field's own elements, -∫ J·∇h over its domains (h one on the
    /// terminal's nodes): their triangles straight, an arc's chords, where
    /// that physics' are curved - its section smaller. NaN where the
    /// terminal is on none of its domains.
    double carriedCurrent(const Coil& c) const;
    void publishConductivity();
    bool solveHarmonic(const Options3& o, QString* error);
    bool sweepFrequencies(const Options3& o, const std::vector<double>& frequencies, double tolerance, const std::function<bool(int)>& each,
                          QString* error) override;
    /// The frequency domain's system, whatever ω: its conductors and
    /// equations, K0 and K1 (the matrix K0 + jω K1), its right-hand side
    /// b(ω) = bc + jω bj, its ports' (1 A each, ω's none), and what makes
    /// its results.
    struct Harmonic {
        Conductors h;
        Equations q;
        double gaugeScale = 1;
        std::vector<double> held;   // by edge dof: A's held value
        std::vector<std::vector<std::array<Expression, 3>>> unit;   // by coil: turns' J at 1 A
        Eigen::SparseMatrix<cd> K0, K1;
        Eigen::VectorXcd bc, bj;
        std::vector<int> ports;     // the coils fed a current
        Eigen::MatrixXcd P;         // by port: its right-hand side (none: not swept)
        int oV = 0, n = 0;
        double assembled = 0;       // s
        /// The iterative solve's, made at its first: A's edge mass M, the
        /// nodes' gradients and the gauge's free vertices' vector fields Π
        /// in A's free edges, each prism's σ (0 where it does not conduct),
        /// the mass' weight γ - none of ω.
        struct Spaces {
            Eigen::SparseMatrix<double> M, Gn, Pi;   // Gn: the gradients by node (A's free edges' rows)
            Eigen::SparseMatrix<double> Vn;   // order 2: by node, its linear nodes' fields (columns its vertices')
            std::vector<double> sigma;
            double gamma = 0;
        };
        mutable std::shared_ptr<Spaces> spaces;
    };
    bool prepareHarmonic(const Options3& o, Harmonic& hs, QString* error);
    /// A Time Dependent step's step: K0 X + K1 Ẋ = b(t), the frequency
    /// domain's matrices, Ẋ = c0 X - the history (Options3::mass, ::history)
    /// - its values (A, the multiplier, ψ, the coils' ψ) and Ȧ and V
    /// (Field3::internal), the coils' current and voltage now.
    bool solveTransient(const Options3& o, QString* error);
    std::unique_ptr<Harmonic> a_stepping;          // its matrices, a stepping's
    std::unique_ptr<SparseRealLdlt> a_steppingLu;  // K0 + c0 K1's factors
    double a_steppingMass = 0;
    Eigen::VectorXd a_steppingR;                    // its sources' part of b, where none reads t
    bool a_steppingVaries = true;                   // one of them reads t: assembled at each step
    std::vector<Eigen::VectorXd> a_steppingVolt;    // by coil fed a voltage: its part of b at 1 V (empty: none)
    std::vector<Eigen::VectorXd> a_steppingTurns;   // by coil of turns: its part of b at 1 A (empty: none)
    std::vector<double> a_steppingWire;             // by coil: its wire's resistance (turns')
    std::vector<char> a_steppingWireVaries;         // by coil: its σw reads t or a field (taken at each step)
    bool a_steppingMassless = false;                // K1 nought: nothing conducts
    bool a_inTime = false;                          // solved last in time
    /// [the solution, each port's] at ω, by a factorization of K0 + jω K1;
    /// and (\a moments 1, 2) their first and second derivatives in ω after
    /// them, each a solve of the same factors - x' = A⁻¹ (b' - j K1 x), x''
    /// = -2 A⁻¹ j K1 x'.
    bool solveExactly(const Options3& o, const Harmonic& hs, double omega, Eigen::MatrixXcd& X, QString* error, int moments = 0);
    /// The same by GMRES (fem3_ams; Options3::solver iterative), its
    /// preconditioner real: an auxiliary space cycle of Re K0 + ω K1 + γ M on
    /// A and the conductors' potential together, and multigrid on the gauge's
    /// multiplier's Schur complement - memory some 1.3 kB an unknown for the
    /// direct solve's factors' 8 kB, its time 10 to 25 times theirs from
    /// 40,000 to 330,000 unknowns.
    bool solveIteratively(const Options3& o, const Harmonic& hs, double omega, Eigen::MatrixXcd& X, QString* error, int moments);
    void buildSpaces(const Harmonic& hs) const;
    /// The field, the coils' results and the impedance matrix of solutions
    /// \a X ([the solution, each port's]) at ω.
    void finishHarmonic(const Options3& o, const Harmonic& hs, double omega, const Eigen::MatrixXcd& X);
    void setHarmonicGlobals();
    /// The harmonic system, (K + jωσM) A + jω C ψ + Bᵀ λ = J, jω (Cᵀ A +
    /// L ψ) = the currents held (each terminal's equation's), B A = 0, is
    /// K0 + jω K1 - its matrix affine in ω, each conductor's block jω's
    /// (ψ = V/(jω)). \a parts 1 assembles K0 (the curl-curl, the gauge) and
    /// the sources J (and the surface currents where \a sheets); 2 K1 (σ's
    /// blocks). Both of the same pattern, explicit noughts kept: K0 + jω K1
    /// a sum of their values. \a r: the sources (1) less what the held
    /// values give - \a held by A's edge dof, \a heldV (volts: K1 ψ jω = K1
    /// V) by node; \a M the matrix where \a matrix.
    void assembleHarmonic(const Equations& q, const Conductors& h, int parts, const std::vector<double>& held, const std::vector<cd>& heldV,
                          const std::vector<std::array<Expression, 3>>& J, bool matrix, bool sheets, Eigen::SparseMatrix<cd>& M,
                          Eigen::VectorXcd& r, double gaugeScale) const;
    double gaugeScaleOf() const;
    /// A warning in the log where a conductor's surface's mesh is coarser
    /// than its skin depth at ω.
    void skinDepthCheck(const Conductors& h, double omega);
    bool conducts(int d) const
    {
        if (d < 0 || d >= int(a_sigma.size())) return false;
        for (const Expression& e : a_sigma[std::size_t(d)])
            if (e.isValid()) return true;
        return false;
    }
    /// The source J by domain: the External Current Densities', and each
    /// coil's N I0 / I_ec times the currents physics' J (\a only: that coil
    /// alone at 1 A, nothing else).
    bool buildSources(QString* error);
    std::vector<std::array<Expression, 3>> sourcesOf(int only, bool turns = true) const;
    /// sourcesOf(-1) without the coils of turns (a step's: theirs apart -
    /// their term reads their currents physics' J, a field, which varies to
    /// Solution3::varies()).
    std::vector<std::array<Expression, 3>> sourcesWithoutTurns() const { return sourcesOf(-1, false); }
    /// Newton's system at A (\a a, by edge dof): its matrix (the tangent,
    /// and the gauge's rows and columns scaled), and the residual.
    void assemble(const Equations& q, const std::vector<double>& a, const std::vector<std::array<Expression, 3>>& J, bool magnets,
                  Eigen::SparseMatrix<double>& M, Eigen::VectorXd& r, double gaugeScale) const;
    double evalAt(const Expression& e, const Vec3& x) const;

    std::vector<FaceCondition> a_faces;                                // by face
    std::vector<std::vector<std::array<Expression, 3>>> a_sheets;      // by face
    std::vector<std::vector<std::array<Expression, 3>>> a_external;    // by domain
    std::vector<Coil> a_coils;
    /// A Force Calculation: its domains, the point its torque is about and
    /// the axis its torque along is of.
    struct ForceCalc {
        QString name;
        std::vector<char> on;    // by domain
        Vec3 point, axis{0, 0, 1};
    };
    std::vector<ForceCalc> a_forces;
    /// Each Force Calculation's force and torque (about its point) by
    /// Maxwell's stress tensor over its domains' faces with another's - the
    /// field read on the other side -, its time average in the frequency
    /// domain; set as globals.
    void setForceGlobals(bool harmonic);
    /// A coil of turns' wire resistance: ∫ |j1|² / (σw fill) over it, j1 its
    /// current density at 1 A (0: none given; a single conductor's its
    /// field's own).
    double wireResistance(int c) const;
    bool a_nonlinear = false;
    /// The frequency domain's effective µr at each quadrature point (by prism,
    /// then its rule's point), a B-H curve's (of its B's amplitude, by a
    /// bisection solved in turn); 0 elsewhere; empty: none.
    std::vector<double> a_muEff;
    bool a_magnets = false;
    double a_energy = 0;
    std::vector<std::vector<double>> a_inductance;   // coils swept at 1 A each: [i][j]
    std::vector<std::array<Expression, 9>> a_sigma;   // by domain: σ, the frequency domain's (invalid: none)
    // The frequency domain's, as last solved: ω (0 at rest), by coil its
    // potential U, its current and its flux linkage (turns'), the coils fed
    // a current (its ports) and their impedance matrix, the losses (resistive
    // and magnetic) and the energy (time averages).
    double a_omega = 0;
    bool a_harmonic = false;   // its sources a frequency domain's (a single conductor's found)
    std::vector<cd> a_U, a_I, a_flux;
    std::vector<int> a_ports;
    std::vector<std::vector<cd>> a_Z;
    double a_losses = 0, a_magneticLosses = 0;
    // The last factorization, and the matrix it is of: a matrix the same at
    // the next solve - a cycle's next pass, a step in time's next step at
    // rest, of a linear model - solved with it, not factorized again (a coil's 1.7 M unknowns factorized twice, 2739
    // s and 717 s, for one solve).
    std::unique_ptr<SparseRealLdlt> a_lu;
    Eigen::SparseMatrix<double> a_factored;
};

/// The same matrix: its pattern and its values, bit for bit (both
/// compressed, as setFromTriplets leaves them).
bool sameMatrix(const Eigen::SparseMatrix<double>& a, const Eigen::SparseMatrix<double>& b)
{
    if (a.rows() != b.rows() || a.cols() != b.cols() || a.nonZeros() != b.nonZeros() || !a.isCompressed() || !b.isCompressed()) return false;
    const auto nz = std::size_t(a.nonZeros());
    return std::equal(a.outerIndexPtr(), a.outerIndexPtr() + a.cols() + 1, b.outerIndexPtr())
           && std::equal(a.innerIndexPtr(), a.innerIndexPtr() + nz, b.innerIndexPtr()) && std::equal(a.valuePtr(), a.valuePtr() + nz, b.valuePtr());
}

// ------------------------------------------------------------------ Setup

bool MagneticProblem3::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field3& f = field();
    const Model& model = a_s.model();
    const Topology3& topo = a_s.topology();
    const std::size_t D = topo.domains.size(), F = topo.faces.size();
    QString why;
    const QVector<int> domains = resolveSelection3(topo, model, Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(D, 0);
    for (int d : domains)
        if (d >= 0 && d < int(D)) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    const std::vector<const Node*> material = materials();
    Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    coef.c.assign(D, {});
    coef.isotropic.assign(D, 1);
    coef.sources.assign(D, {});
    coef.sourceScale.assign(D, 1.0);
    coef.capacity.assign(D, Expression());
    coef.named.clear();
    for (const char* k : {"Brx", "Bry", "Brz", "Jx", "Jy", "Jz", "murIm"}) coef.named[QString::fromLatin1(k)].assign(D, Expression());
    for (int k = 0; k < 9; ++k) coef.named[QStringLiteral("sigma%1").arg(k)].assign(D, Expression());
    coef.curves.assign(D, nullptr);
    a_faces.assign(F, FaceCondition());
    a_sheets.assign(F, {});
    a_external.assign(D, {});
    a_coils.clear();
    a_forces.clear();
    a_nonlinear = false;
    a_magnets = false;
    a_sigma.assign(D, {});
    const Scope& params = a_s.parameters().scope;
    std::vector<const Node*> ampereOf(D, nullptr);
    std::vector<const Node*> magnetOf(D, nullptr);
    int order = 0;
    const auto vector3 = [this](const Node& feature, const char* const keys[3], const QString& zero, std::array<Expression, 3>* out) {
        bool ok = true;
        for (int c = 0; c < 3; ++c) {
            const QString key = QString::fromLatin1(keys[c]);
            const QString text = feature.text(key);
            (*out)[std::size_t(c)] = compile(text.trimmed().isEmpty() ? zero : text, feature, key);
            ok = ok && (*out)[std::size_t(c)].isValid();
        }
        return ok;
    };
    for (const Node& feature : a_node->children) {
        ++order;
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k) continue;
        const QVector<int> picked = resolveSelection3(topo, model, k->selection, feature.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(feature.name(), why));
        const QString t = feature.type;
        if (t == QLatin1String("magnetic/ampere")) {
            for (int d : picked)
                if (d >= 0 && d < int(D)) ampereOf[std::size_t(d)] = &feature;
        } else if (t == QLatin1String("magnetic/magnet")) {
            for (int d : picked)
                if (d >= 0 && d < int(D)) magnetOf[std::size_t(d)] = &feature;
        } else if (t == QLatin1String("magnetic/current")) {
            static const char* const keys[3] = {"Jex", "Jey", "Jez"};
            std::array<Expression, 3> j;
            if (vector3(feature, keys, QStringLiteral("0[A/m^2]"), &j))
                for (int d : picked)
                    if (d >= 0 && d < int(D)) a_external[std::size_t(d)].push_back(j);
        } else if (t == QLatin1String("magnetic/force")) {
            ForceCalc fc;
            fc.name = feature.text(QStringLiteral("name")).trimmed();
            if (fc.name.isEmpty()) fc.name = feature.name();
            fc.on.assign(D, 0);
            for (int d : picked)
                if (d >= 0 && d < int(D)) fc.on[std::size_t(d)] = 1;
            double c[6] = {0, 0, 0, 0, 0, 1};
            static const char* const keys[6] = {"x0", "y0", "z0", "ax", "ay", "az"};
            for (int k = 0; k < 6; ++k) {
                const QString text = feature.text(QString::fromLatin1(keys[k])).trimmed();
                if (text.isEmpty()) continue;
                const Expression e = compile(text, feature, QString::fromLatin1(keys[k]), false);
                if (e.isValid() && e.isConstant()) c[k] = e.constant();
                else if (e.isValid()) problem(tr("%1: its %2 must be a constant").arg(feature.name(), QString::fromLatin1(keys[k])));
            }
            fc.point = Vec3{c[0], c[1], c[2]};
            const double len = std::sqrt(c[3] * c[3] + c[4] * c[4] + c[5] * c[5]);
            if (!(len > 0)) problem(tr("%1: its axis is nought").arg(feature.name()));
            else fc.axis = Vec3{c[3] / len, c[4] / len, c[5] / len};
            a_forces.push_back(fc);
        } else if (t == QLatin1String("magnetic/coil")) {
            Coil c;
            c.name = feature.text(QStringLiteral("name"));
            if (c.name.trimmed().isEmpty()) c.name = feature.name();
            c.on.assign(D, 0);
            for (int d : picked)
                if (d >= 0 && d < int(D)) c.on[std::size_t(d)] = 1;
            c.current = compile(feature.text(QStringLiteral("I0")), feature, QStringLiteral("I0"), false);
            c.single = feature.text(QStringLiteral("conductor")) != QLatin1String("multi");
            const QString role = feature.text(QStringLiteral("role"));
            if (c.single && role == QLatin1String("voltage")) c.role = Coil::Voltage;
            else if (c.single && role == QLatin1String("floating")) c.role = Coil::Floating;
            else if (c.single && role == QLatin1String("grounded")) c.role = Coil::Grounded;
            if (c.role == Coil::Voltage) c.voltage = compile(feature.text(QStringLiteral("V0")), feature, QStringLiteral("V0"), false);
            for (const Coil& other : a_coils)
                if (other.name == c.name) problem(tr("%1: another coil is named %2").arg(feature.name(), c.name));
            if (!c.single) {
                // (Its wire's resistance: ∫ |j1|² / (σw fill), j1 its current
                // density at 1 A.)
                const QString sw = feature.text(QStringLiteral("sigmaw")).trimmed();
                if (!sw.isEmpty()) {
                    c.wireSigma = compile(sw, feature, QStringLiteral("sigmaw"));
                    const QString fl = feature.text(QStringLiteral("fill")).trimmed();
                    if (!fl.isEmpty()) {
                        const Expression fe = compile(fl, feature, QStringLiteral("fill"), false);
                        if (fe.isValid() && (!fe.isConstant() || !(fe.constant() > 0 && fe.constant() <= 1)))
                            problem(tr("%1: its fill factor must be a constant over 0, 1 at most").arg(feature.name()));
                        else if (fe.isValid()) c.fill = fe.constant();
                    }
                }
            }
            const QString n = !c.single ? feature.text(QStringLiteral("N")) : QStringLiteral("1");
            c.turns = compile(n.trimmed().isEmpty() ? QStringLiteral("1") : n, feature, QStringLiteral("N"), false);
            const QStringList tags = feature.list(QStringLiteral("ec"));
            c.ec = tags.isEmpty() ? QStringLiteral("ec") : tags.first();
            c.terminal = feature.text(QStringLiteral("terminal")).trimmed();
            const int fi = a_s.fieldIndex(c.ec);
            if (fi < 0 || a_s.fields()[std::size_t(fi)].type != QLatin1String("currents")) {
                problem(tr("%1: its current density is a currents physics' (%2), which the model does not solve").arg(feature.name(), c.ec));
                continue;
            }
            a_reads.insert(fi);
            a_coils.push_back(c);
        } else if (t == QLatin1String("magnetic/insulation") || t == QLatin1String("magnetic/potential") || t == QLatin1String("magnetic/pmc")) {
            FaceCondition c;
            c.order = order;
            c.kind = t == QLatin1String("magnetic/insulation") ? FaceCondition::Insulation
                     : t == QLatin1String("magnetic/potential") ? FaceCondition::Potential
                                                                : FaceCondition::Free;
            if (c.kind == FaceCondition::Potential) {
                static const char* const keys[3] = {"A0x", "A0y", "A0z"};
                std::array<Expression, 3> a0;
                if (!vector3(feature, keys, QStringLiteral("0[Wb/m]"), &a0)) continue;
                for (int i = 0; i < 3; ++i) c.a0[i] = a0[std::size_t(i)];
            }
            for (int b : picked)
                if (b >= 0 && b < int(F)) a_faces[std::size_t(b)] = c;
        } else if (t == QLatin1String("magnetic/surfacecurrent")) {
            static const char* const keys[3] = {"Jsx", "Jsy", "Jsz"};
            std::array<Expression, 3> js;
            if (vector3(feature, keys, QStringLiteral("0[A/m]"), &js))
                for (int b : picked)
                    if (b >= 0 && b < int(F)) a_sheets[std::size_t(b)].push_back(js);
        }
    }
    // Each domain's permeability: a value, a tensor in the model's axes (a
    // material's turned into them), or a B-H curve.
    for (int d = 0; d < int(D); ++d) {
        if (!f.domains[std::size_t(d)]) continue;
        const Node* feat = ampereOf[std::size_t(d)];
        const QString object = topo.objectNames.value(topo.domains[std::size_t(d)].owner);
        if (!feat) {
            problem(tr("domain %1 (%2) has no Ampère's Law feature").arg(d + 1).arg(object));
            continue;
        }
        const bool user = feat->text(QStringLiteral("source")) == QLatin1String("user");
        const Node* m = material[std::size_t(d)];
        if (!user && !m) {
            problem(tr("domain %1 (%2) has no material: its relative permeability is needed").arg(d + 1).arg(object));
            continue;
        }
        std::array<Expression, 9>& c = coef.c[std::size_t(d)];
        const Expression zero = Expression::compile(QStringLiteral("0"), a_s.pointScope());
        if (feat->text(QStringLiteral("constitutive")) == QLatin1String("bh")) {
            QString cwhy;
            const QJsonValue table = user ? feat->value(QStringLiteral("BH")) : detail::materialValue(*m, QStringLiteral("BH"));
            if (table.isUndefined() || table.isNull() || (table.isArray() && table.toArray().isEmpty())
                || (table.isString() && table.toString().trimmed().isEmpty())) {
                problem(user ? tr("%1: its B-H curve is empty").arg(feat->name())
                             : tr("domain %1 (%2): its material, %3, has no B-H curve").arg(d + 1).arg(object, m->name()));
                continue;
            }
            coef.curves[std::size_t(d)] = bhCurveOf(table, params, &cwhy);
            if (!coef.curves[std::size_t(d)]) {
                problem(tr("domain %1 (%2): %3").arg(d + 1).arg(object, cwhy));
                continue;
            }
            // (µr, as a variable reads it: the curve's B/(µ0 H) at its first
            // slope.)
            const double mu = 1 / (Mu0 * coef.curves[std::size_t(d)]->nu(0));
            for (int i = 0; i < 9; ++i) c[std::size_t(i)] = i % 4 == 0 ? Expression::compile(QString::number(mu, 'g', 17), a_s.pointScope()) : zero;
            a_nonlinear = true;
        } else if (user) {
            const QString aniso = feat->text(QStringLiteral("anisotropy"));
            static const char axes[] = "xyz";
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    QString key, text;
                    if (aniso == QLatin1String("diagonal") || aniso == QLatin1String("symmetric")) {
                        if (i != j && aniso != QLatin1String("symmetric")) {
                            c[std::size_t(3 * i + j)] = zero;
                            continue;
                        }
                        key = QStringLiteral("mur") + QLatin1Char(axes[std::min(i, j)]) + QLatin1Char(axes[std::max(i, j)]);
                        text = feat->text(key);
                        if (text.trimmed().isEmpty()) text = i == j ? QStringLiteral("1") : QStringLiteral("0");
                    } else {
                        key = QStringLiteral("mur");
                        text = i == j ? feat->text(key) : QStringLiteral("0");
                    }
                    c[std::size_t(3 * i + j)] = text == QLatin1String("0") ? zero : compile(text, *feat, key);
                    if (i != j && text != QLatin1String("0")) coef.isotropic[std::size_t(d)] = 0;
                }
            if (aniso == QLatin1String("diagonal") || aniso == QLatin1String("symmetric")) coef.isotropic[std::size_t(d)] = 0;
        } else {
            const QString text = detail::Problem::materialText(*m, QStringLiteral("mur"), &params);
            if (text.trimmed().isEmpty()) {
                problem(tr("domain %1 (%2): its material, %3, has no relative permeability").arg(d + 1).arg(object, m->name()));
                continue;
            }
            std::optional<TensorText> t = tensorText(text, &why);
            if (!t) {
                problem(tr("%1's relative permeability: %2").arg(m->name(), why));
                continue;
            }
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    c[std::size_t(3 * i + j)] = t->c[i][j] == QLatin1String("0") ? zero : compile(t->c[i][j], *m, QStringLiteral("mur"));
            coef.isotropic[std::size_t(d)] = t->anisotropic() ? 0 : 1;
        }
        // A permanent magnet: Br along its direction (its recoil
        // permeability Ampère's Law's µr).
        if (const Node* mg = magnetOf[std::size_t(d)]) {
            if (coef.curves[std::size_t(d)]) {
                problem(tr("%1: a magnet of a B-H curve (its µr is the recoil one, a value)").arg(mg->name()));
                continue;
            }
            const bool fromUser = mg->text(QStringLiteral("source")) == QLatin1String("user");
            const QString br = fromUser ? mg->text(QStringLiteral("Br")) : m ? detail::Problem::materialText(*m, QStringLiteral("Br"), &params) : QString();
            if (br.trimmed().isEmpty()) {
                problem(tr("%1: its remanent flux density Br is not given").arg(mg->name()));
                continue;
            }
            double dir[3] = {0, 0, 0};
            static const char* const keys[3] = {"dx", "dy", "dz"};
            for (int i = 0; i < 3; ++i) {
                const std::optional<double> v = evaluateConstant(mg->text(QString::fromLatin1(keys[i])).trimmed().isEmpty() ? QStringLiteral("0") : mg->text(QString::fromLatin1(keys[i])),
                                                                 params, Dim(), &why);
                if (!v) {
                    problem(tr("%1: its direction: %2").arg(mg->name(), why));
                    continue;
                }
                dir[i] = *v;
            }
            const double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (!(len > 0)) {
                problem(tr("%1: its direction (dx, dy, dz) is nought").arg(mg->name()));
                continue;
            }
            static const char* const named[3] = {"Brx", "Bry", "Brz"};
            for (int i = 0; i < 3; ++i)
                coef.named[QString::fromLatin1(named[i])][std::size_t(d)] =
                    Expression::compile(QStringLiteral("(%1)*%2").arg(br).arg(dir[i] / len, 0, 'g', 17), a_s.pointScope());
            a_magnets = true;
        }
    }
    // Each domain's conductivity σ (a value, or a tensor in the model's axes)
    // and µr'' (µr = µr' - j µr'', a core's losses), the frequency domain's:
    // Ampère's Law's, or its material's. None in a coil of turns (no eddy
    // currents in them).
    std::vector<char> turns(D, 0);
    for (const Coil& c : a_coils)
        if (!c.single)
            for (std::size_t d = 0; d < D; ++d) turns[d] = turns[d] || c.on[d];
    for (int d = 0; d < int(D); ++d) {
        const Node* feat = ampereOf[std::size_t(d)];
        if (!f.domains[std::size_t(d)] || !feat) continue;
        const bool user = feat->text(QStringLiteral("source")) == QLatin1String("user");
        const Node* m = material[std::size_t(d)];
        if (!turns[std::size_t(d)]) {
            const QString text = user ? feat->text(QStringLiteral("sigma")) : m ? detail::Problem::materialText(*m, QStringLiteral("sigma"), &params) : QString();
            if (!text.trimmed().isEmpty()) {
                std::optional<TensorText> t = tensorText(text, &why);
                if (!t) {
                    problem(tr("%1's electrical conductivity: %2").arg(user ? feat->name() : m->name(), why));
                    continue;
                }
                bool any = false;
                std::array<Expression, 9> sg;
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) {
                        const QString& c = t->c[i][j];
                        if (c == QLatin1String("0")) continue;
                        sg[std::size_t(3 * i + j)] = compile(c, user ? *feat : *m, QStringLiteral("sigma"));
                        const Expression& e = sg[std::size_t(3 * i + j)];
                        any = any || !e.isValid() || !e.isConstant() || e.constant() != 0;
                    }
                if (any) a_sigma[std::size_t(d)] = sg;
            }
        }
        if (coef.curves[std::size_t(d)]) continue;
        const QString im = user ? feat->text(QStringLiteral("murIm")) : m ? detail::Problem::materialText(*m, QStringLiteral("murIm"), &params) : QString();
        if (!im.trimmed().isEmpty() && im.trimmed() != QLatin1String("0"))
            coef.named[QStringLiteral("murIm")][std::size_t(d)] = compile(im, user ? *feat : *m, QStringLiteral("murIm"));
    }
    publishConductivity();
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    return true;
}

void MagneticProblem3::publishConductivity()
{
    Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    for (int k = 0; k < 9; ++k) {
        std::vector<Expression>& v = coef.named[QStringLiteral("sigma%1").arg(k)];
        v.assign(a_sigma.size(), Expression());
        for (std::size_t d = 0; d < a_sigma.size(); ++d) v[d] = a_sigma[d][std::size_t(k)];
    }
}

double MagneticProblem3::evalAt(const Expression& e, const Vec3& x) const
{
    if (!e.isValid()) return NaN;
    if (e.isConstant()) return e.constant();
    std::vector<double> values(std::size_t(a_s.pointScope().slotCount()), NaN);
    for (int slot : e.variables()) {
        const Solution3::VariableDef& d = a_s.definitions()[std::size_t(slot)];
        using K = Solution3::VariableDef::Kind;
        values[std::size_t(slot)] = d.kind == K::X ? x.x : d.kind == K::Y ? x.y : d.kind == K::Z ? x.z : d.kind == K::Time ? a_s.time() : NaN;
    }
    return e.eval(values.data());
}

// --------------------------------------------------------------- Sources

std::vector<std::array<Expression, 3>> MagneticProblem3::sourcesOf(int only, bool turns) const
{
    const std::size_t D = a_s.topology().domains.size();
    std::vector<std::array<Expression, 3>> out(D);
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    static const char* const axes[3] = {"x", "y", "z"};
    for (std::size_t d = 0; d < D; ++d) {
        if (!f.domains[d]) continue;
        for (int c = 0; c < 3; ++c) {
            QStringList terms;
            if (only < 0)
                for (const auto& j : a_external[d]) terms << QStringLiteral("(") + j[std::size_t(c)].text() + QLatin1Char(')');
            for (int k = 0; k < int(a_coils.size()); ++k) {
                const Coil& coil = a_coils[std::size_t(k)];
                if (!coil.on[d] || (only >= 0 && only != k) || (!turns && !coil.single)) continue;
                // (In the frequency domain a single conductor's current is
                // found, not given.)
                if (a_harmonic && coil.single) continue;
                // (A coil alone at 1 A: N / I_ec times its J. A coil of turns'
                // current its own expression, read where and when it is
                // read - a results' snapshot in time its own t: a number, as
                // a single conductor's at rest, is one time's, the
                // coefficients not kept by snapshot.)
                if (only < 0 && !coil.single && coil.current.isValid())
                    terms << QStringLiteral("%1*((%2)/1[A])*%3.J%4").arg(coil.perAmp, 0, 'g', 17).arg(coil.current.text(), coil.ec, QString::fromLatin1(axes[c]));
                else {
                    const double scale = only >= 0 ? coil.perAmp : coil.scale;
                    terms << QStringLiteral("%1*%2.J%3").arg(scale, 0, 'g', 17).arg(coil.ec, QString::fromLatin1(axes[c]));
                }
            }
            if (!terms.isEmpty()) out[d][std::size_t(c)] = Expression::compile(terms.join(QLatin1String(" + ")), a_s.pointScope());
        }
    }
    return out;
}

bool MagneticProblem3::buildSources(QString* error)
{
    // Each coil's scale: N I0 over the current its terminal carries in the
    // currents physics as solved.
    for (Coil& c : a_coils) {
        const int fi = a_s.fieldIndex(c.ec);
        const Field3& ec = a_s.fields()[std::size_t(fi)];
        const double i0 = evalAt(c.current, Vec3{}), n = evalAt(c.turns, Vec3{});
        double through = NaN;
        for (const Field3::Terminal& t : ec.terminals)
            if (c.terminal.isEmpty() || t.name == c.terminal) {
                through = t.charge;
                break;
            }
        if (!ec.solved && std::isnan(through)) {
            c.scale = c.perAmp = 0;   // (not yet solved: a coupled round's first)
            continue;
        }
        if (!std::isfinite(through) || through == 0) {
            *error = tr("coil %1: its currents physics %2 has no terminal %3 carrying a current").arg(c.name, c.ec, c.terminal.isEmpty() ? tr("(any)") : c.terminal);
            return false;
        }
        // (Its current as this field's elements carry it: their straight
        // section's, where the currents physics' own is curved - a round
        // wire's 0.7 % less at 0.1 mm, its L 0.9 % low with the physics'
        // own.)
        const double carried = carriedCurrent(c);
        if (std::isfinite(carried) && carried * through > 0) {
            if (std::abs(carried / through - 1) > 1e-6)
                a_s.log << tr("%1: coil %2's current, %3 A in %4, is %5 A through this field's elements (their triangles straight): N I0 "
                              "over the latter")
                               .arg(a_node->name(), c.name)
                               .arg(through, 0, 'g', 6)
                               .arg(c.ec)
                               .arg(carried, 0, 'g', 6);
            through = carried;
        }
        c.perAmp = n / through;
        c.scale = c.perAmp * i0;
    }
    Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const std::vector<std::array<Expression, 3>> J = sourcesOf(-1);
    static const char* const named[3] = {"Jx", "Jy", "Jz"};
    for (std::size_t d = 0; d < J.size(); ++d)
        for (int c = 0; c < 3; ++c) coef.named[QString::fromLatin1(named[c])][d] = J[d][std::size_t(c)];
    return true;
}

// ------------------------------------------------------------- Equations

bool MagneticProblem3::buildEquations(Equations& q, QString* error)
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    q = Equations();
    q.eqA.assign(std::size_t(es.size()), -1);
    q.eqP.assign(std::size_t(ns.size()), -1);
    q.fixedA.assign(std::size_t(es.size()), 0.0);
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        const int* ed = es.dofs(e);
        for (int i = 0; i < es.perElement(); ++i)
            if (ed[i] >= 0) q.eqA[std::size_t(ed[i])] = -2;
        const int* nd = ns.dofs(e);
        for (int i = 0; i < ns.perElement(); ++i) q.eqP[std::size_t(nd[i])] = -2;
    }
    // A face's tangential A held: its edge dofs (nought, or a potential's
    // projection), and the gauge's multiplier nought on it.
    const auto edgeDofs = [&mesh, &es](int fi) {
        const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];
        if (!mf.quad) return es.floorDofs(mf.plan, mf.height);
        const Mesh::Edge& pe = mesh.plan->edges[std::size_t(mf.plan)];
        return es.wallDofs(pe.a, pe.b, mf.height);
    };
    std::vector<std::pair<int, int>> held;   // (order, mesh face)
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
        const int face = mesh.faces[std::size_t(fi)].face;
        const FaceCondition& c = a_faces[std::size_t(face)];
        if (c.kind == FaceCondition::Free) continue;
        // (Insulation on the physics' outer faces alone: "all" boundaries,
        // its default, is not the faces between its domains.)
        const Topology3::Face& tf = a_s.topology().faces[std::size_t(face)];
        const auto in = [&f](int d) { return d >= 0 && d < int(f.domains.size()) && f.domains[std::size_t(d)]; };
        if (c.kind == FaceCondition::Insulation && in(tf.back) && in(tf.front)) continue;
        held.emplace_back(c.order, fi);
    }
    std::stable_sort(held.begin(), held.end());
    std::set<int> potential;   // edge dofs a potential holds
    std::vector<int> potentialFaces;
    for (const auto& [ord, fi] : held) {
        (void)ord;
        const bool held0 = a_faces[std::size_t(mesh.faces[std::size_t(fi)].face)].kind != FaceCondition::Potential;
        bool on = true;
        for (int dof : ns.faceDofs(fi)) on = on && dof >= 0 && q.eqP[std::size_t(dof)] != -1;
        if (!on) continue;
        for (int dof : edgeDofs(fi)) {
            if (dof < 0 || q.eqA[std::size_t(dof)] == -1) continue;
            q.eqA[std::size_t(dof)] = -3;
            if (!held0) potential.insert(dof);
            else potential.erase(dof);
        }
        for (int dof : ns.faceDofs(fi))
            if (dof >= 0) q.eqP[std::size_t(dof)] = -3;
        if (!held0) potentialFaces.push_back(fi);
    }
    // A potential's edge dofs: the projection of n × A0 on the faces' edge
    // functions' tangential traces, ∫ (n × Wi)·(n × Wj) x = ∫ (n × Wi)·(n ×
    // A0).
    if (!potential.empty()) {
        std::vector<int> local(std::size_t(es.size()), -1);
        int np = 0;
        for (int dof : potential) local[std::size_t(dof)] = np++;
        using T = Eigen::Triplet<double>;
        std::vector<T> tp;
        Eigen::VectorXd rhs = Eigen::VectorXd::Zero(np);
        const int degree = es.order() == 1 ? 2 : 4;
        PointEvaluator3 ev(a_s);
        for (int fi : potentialFaces) {
            const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];
            const FaceCondition& c = a_faces[std::size_t(mf.face)];
            const Topology3::Face& tf = a_s.topology().faces[std::size_t(mf.face)];
            const int side = tf.back >= 0 && f.domains[std::size_t(tf.back)] ? tf.back : tf.front;
            for (const Space3::FacePoint& fp : ns.facePoints(fi, degree, side)) {
                if (fp.prism < 0) continue;
                const Vec3 n = outwardNormal3(ns, mf, fp);
                EdgeSpace3::Basis b;
                es.basis(fp.prism, fp.xi, fp.eta, fp.zeta, b);
                double a0[3];
                for (int k = 0; k < 3; ++k) {
                    a0[k] = evalAt(c.a0[k], fp.x);
                    if (!std::isfinite(a0[k])) a0[k] = 0;
                }
                const auto cross = [&n](const double v[3], double out[3]) {
                    out[0] = n.y * v[2] - n.z * v[1];
                    out[1] = n.z * v[0] - n.x * v[2];
                    out[2] = n.x * v[1] - n.y * v[0];
                };
                double na0[3];
                cross(a0, na0);
                const int* ed = es.dofs(fp.prism);
                for (int i = 0; i < b.count; ++i) {
                    const int li = ed[i] >= 0 ? local[std::size_t(ed[i])] : -1;
                    if (li < 0) continue;
                    double ni[3];
                    cross(b.v[i], ni);
                    rhs(li) += fp.weight * (ni[0] * na0[0] + ni[1] * na0[1] + ni[2] * na0[2]);
                    for (int j = 0; j < b.count; ++j) {
                        const int lj = ed[j] >= 0 ? local[std::size_t(ed[j])] : -1;
                        if (lj < 0) continue;
                        double nj[3];
                        cross(b.v[j], nj);
                        tp.emplace_back(li, lj, fp.weight * (ni[0] * nj[0] + ni[1] * nj[1] + ni[2] * nj[2]));
                    }
                }
            }
        }
        Eigen::SparseMatrix<double> P(np, np);
        P.setFromTriplets(tp.begin(), tp.end());
        SparseLu<double> lu;
        lu.compute(P);
        if (lu.info() != Eigen::Success) {
            *error = tr("%1: the potential held on its faces could not be projected").arg(a_node->name());
            return false;
        }
        const Eigen::VectorXd x = lu.solve(rhs);
        for (int dof : potential) q.fixedA[std::size_t(dof)] = x(local[std::size_t(dof)]);
    }
    for (std::size_t k = 0; k < q.eqA.size(); ++k)
        if (q.eqA[k] == -2) q.eqA[k] = q.nA++;
    // (No face holds the multiplier: one node's held, its constant.)
    bool anyHeld = false;
    for (int v : q.eqP) anyHeld = anyHeld || v == -3;
    for (std::size_t k = 0; k < q.eqP.size(); ++k)
        if (q.eqP[k] == -2) {
            if (!anyHeld) {
                q.eqP[k] = -3;
                anyHeld = true;
                continue;
            }
            q.eqP[k] = q.nP++;
        }
    if (q.nA == 0) {
        *error = tr("%1 has nothing to solve").arg(a_node->name());
        return false;
    }
    return true;
}

// --------------------------------------------------------------- Assembly

void MagneticProblem3::assemble(const Equations& q, const std::vector<double>& a, const std::vector<std::array<Expression, 3>>& J, bool magnets,
                                Eigen::SparseMatrix<double>& M, Eigen::VectorXd& r, double gaugeScale) const
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const int nA = q.nA, n = q.nA + q.nP;
    using T = Eigen::Triplet<double>;
    std::vector<T> tm;
    r = Eigen::VectorXd::Zero(n);
    const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    const int nb = es.perElement(), npn = ns.perElement();
    tm.reserve(std::size_t(es.elements()) * std::size_t(nb * nb + 2 * nb * npn));
    PointEvaluator3 ev(a_s);
    std::vector<double> Ke(static_cast<std::size_t>(nb * nb)), Be(static_cast<std::size_t>(npn * nb)), Fe(static_cast<std::size_t>(nb));
    static const char* const brKeys[3] = {"Brx", "Bry", "Brz"};
    const std::vector<Expression>* br[3];
    for (int c = 0; c < 3; ++c) br[c] = &coef.named.at(QString::fromLatin1(brKeys[c]));
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        const std::array<Expression, 9>& mur = coef.c[std::size_t(d)];
        const std::shared_ptr<const BHCurve>& curve = coef.curves[std::size_t(d)];
        bool constant = !curve;
        for (const Expression& x : mur) constant = constant && (!x.isValid() || x.isConstant());
        double nu[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        if (constant) {
            double m9[9];
            for (int k = 0; k < 9; ++k) m9[k] = mur[std::size_t(k)].isValid() ? mur[std::size_t(k)].constant() : 0;
            reluctivity(m9, nu);
        }
        const bool sourced = J[std::size_t(d)][0].isValid() || J[std::size_t(d)][1].isValid() || J[std::size_t(d)][2].isValid();
        const bool magnet = magnets && (*br[0])[std::size_t(d)].isValid();
        std::fill(Ke.begin(), Ke.end(), 0.0);
        std::fill(Be.begin(), Be.end(), 0.0);
        std::fill(Fe.begin(), Fe.end(), 0.0);
        const int* ed = es.dofs(e);
        const int* nd = ns.dofs(e);
        const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
        const AffineMap am = affineMap(es.planNodes(), t);
        const double h = es.z(layer + 1) - es.z(layer);
        double ae[36];
        for (int i = 0; i < nb; ++i) ae[i] = ed[i] >= 0 ? a[std::size_t(ed[i])] : 0.0;
        for (const auto& qp : rule.points) {
            EdgeSpace3::Basis b;
            es.basis(e, qp[0], qp[1], qp[2], b);
            const double w = qp[3] * b.det;
            if (!constant || sourced || magnet) ev.at(e, qp[0], qp[1], qp[2], b.x);
            if (!constant && !curve) {
                double m9[9];
                for (int k = 0; k < 9; ++k) m9[k] = mur[std::size_t(k)].isValid() ? ev.eval(mur[std::size_t(k)]) : 0;
                reluctivity(m9, nu);
            }
            // B here, H(B) and its tangent dH/dB.
            double B[3] = {0, 0, 0};
            for (int i = 0; i < nb; ++i)
                for (int k = 0; k < 3; ++k) B[k] += b.c[i][k] * ae[i];
            double H[3], Tg[9];
            if (curve) {
                const double bn = std::sqrt(B[0] * B[0] + B[1] * B[1] + B[2] * B[2]);
                const double nub = curve->nu(bn), slope = curve->slope(bn);
                for (int k = 0; k < 3; ++k) H[k] = nub * B[k];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        Tg[3 * i + j] = (i == j ? nub : 0.0) + (bn > 0 ? (slope - nub) * B[i] * B[j] / (bn * bn) : 0.0);
            } else {
                double Br[3] = {0, 0, 0};
                if (magnet)
                    for (int k = 0; k < 3; ++k) {
                        Br[k] = ev.eval((*br[k])[std::size_t(d)]);
                        if (!std::isfinite(Br[k])) Br[k] = 0;
                    }
                for (int i = 0; i < 3; ++i) {
                    H[i] = 0;
                    for (int j = 0; j < 3; ++j) H[i] += nu[3 * i + j] * (B[j] - Br[j]);
                }
                std::copy(nu, nu + 9, Tg);
            }
            double Jv[3] = {0, 0, 0};
            if (sourced)
                for (int k = 0; k < 3; ++k)
                    if (J[std::size_t(d)][std::size_t(k)].isValid()) {
                        Jv[k] = ev.eval(J[std::size_t(d)][std::size_t(k)]);
                        if (!std::isfinite(Jv[k])) Jv[k] = 0;
                    }
            for (int i = 0; i < nb; ++i) {
                // The residual: ∫ J·Wi - ∫ H·curl Wi.
                Fe[std::size_t(i)] += w * (Jv[0] * b.v[i][0] + Jv[1] * b.v[i][1] + Jv[2] * b.v[i][2] - (H[0] * b.c[i][0] + H[1] * b.c[i][1] + H[2] * b.c[i][2]));
                double tc[3];
                for (int k = 0; k < 3; ++k) tc[k] = Tg[3 * k] * b.c[i][0] + Tg[3 * k + 1] * b.c[i][1] + Tg[3 * k + 2] * b.c[i][2];
                for (int j = 0; j < nb; ++j) Ke[std::size_t(i * nb + j)] += w * (b.c[j][0] * tc[0] + b.c[j][1] * tc[1] + b.c[j][2] * tc[2]);
            }
            // The gauge: ∫ ∇qk·Wj, scaled.
            PrismShape s;
            prismShapeAt(ns.order(), qp[0], qp[1], qp[2], s);
            double g[18][3];
            nodalGradients(s, am, h, g);
            for (int k = 0; k < npn; ++k)
                for (int j = 0; j < nb; ++j) Be[std::size_t(k * nb + j)] += w * gaugeScale * (g[k][0] * b.v[j][0] + g[k][1] * b.v[j][1] + g[k][2] * b.v[j][2]);
        }
        for (int i = 0; i < nb; ++i) {
            const int ei = ed[i] >= 0 ? q.eqA[std::size_t(ed[i])] : -1;
            if (ei < 0) continue;
            r(ei) += Fe[std::size_t(i)];
            for (int j = 0; j < nb; ++j) {
                const int ej = ed[j] >= 0 ? q.eqA[std::size_t(ed[j])] : -1;
                if (ej >= 0) tm.emplace_back(ei, ej, Ke[std::size_t(i * nb + j)]);
            }
        }
        for (int k = 0; k < npn; ++k) {
            const int pk = q.eqP[std::size_t(nd[k])];
            for (int j = 0; j < nb; ++j) {
                const int ej = ed[j] >= 0 ? q.eqA[std::size_t(ed[j])] : -1;
                if (ej < 0) continue;
                const double v = Be[std::size_t(k * nb + j)];
                // (Its constraint's residual -B a, and Bᵀψ's: ψ is solved
                // for whole, from nought, each step.)
                if (pk >= 0) {
                    tm.emplace_back(nA + pk, ej, v);
                    tm.emplace_back(ej, nA + pk, v);
                    r(nA + pk) -= v * ae[j];
                }
            }
            // (Held dofs' share of the constraint: B times their values.)
            if (pk >= 0)
                for (int j = 0; j < nb; ++j)
                    if (ed[j] >= 0 && q.eqA[std::size_t(ed[j])] == -3) r(nA + pk) -= Be[std::size_t(k * nb + j)] * ae[j];
        }
    }
    // The surface currents: ∫ Js·Wi on their faces.
    const int degree = es.order() == 1 ? 2 : 4;
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
        const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];
        const auto& sheets = a_sheets[std::size_t(mf.face)];
        if (sheets.empty()) continue;
        const Topology3::Face& tf = a_s.topology().faces[std::size_t(mf.face)];
        const int side = tf.back >= 0 && f.domains[std::size_t(tf.back)] ? tf.back : tf.front;
        for (const Space3::FacePoint& fp : ns.facePoints(fi, degree, side)) {
            if (fp.prism < 0) continue;
            EdgeSpace3::Basis b;
            es.basis(fp.prism, fp.xi, fp.eta, fp.zeta, b);
            ev.at(fp.prism, fp.xi, fp.eta, fp.zeta, fp.x);
            double js[3] = {0, 0, 0};
            for (const auto& s : sheets)
                for (int k = 0; k < 3; ++k) {
                    const double v = ev.eval(s[std::size_t(k)]);
                    if (std::isfinite(v)) js[k] += v;
                }
            const int* ed = es.dofs(fp.prism);
            for (int i = 0; i < b.count; ++i) {
                const int ei = ed[i] >= 0 ? q.eqA[std::size_t(ed[i])] : -1;
                if (ei >= 0) r(ei) += fp.weight * (js[0] * b.v[i][0] + js[1] * b.v[i][1] + js[2] * b.v[i][2]);
            }
        }
    }
    M.resize(n, n);
    M.setFromTriplets(tm.begin(), tm.end());
}

// ------------------------------------------------------------------ Solve

bool MagneticProblem3::solve(const Options3& o, QString* error)
{
    Field3& f = field();
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const QString name = a_node->name();
    // (A Frequency Domain step's: harmonic, eddy currents in its
    // conductors.)
    if (o.frequency > 0) return solveHarmonic(o, error);
    if (o.mass > 0 && o.history && hasMass()) return solveTransient(o, error);
    if (o.inTime && !hasMass())
        for (const Coil& c : a_coils)
            if (!c.single) {
                const QString said = tr("%1: a B-H curve's field in time is solved at rest at each time - coil %2's voltage its wire's R I alone, "
                                        "dΦ/dt not in it (a relative permeability given, the field is stepped and its voltage has it)")
                                         .arg(name, c.name);
                if (!a_s.warnings.contains(said)) {
                    a_s.warnings << said;
                    a_s.log << said;
                }
            }
    a_inTime = false;
    a_omega = 0;
    a_harmonic = false;
    f.terminals.clear();
    a_U.clear();
    a_I.clear();
    a_flux.clear();
    a_Z.clear();
    a_ports.clear();
    if (!buildSources(error)) return false;
    Equations q;
    if (!buildEquations(q, error)) return false;
    const double gaugeScale = gaugeScaleOf();
    // A: its held values, else as it was (Newton's start).
    std::vector<double> a(std::size_t(es.size()), 0.0);
    const std::size_t nE = std::size_t(es.size());
    if (f.values.size() >= nE)
        for (std::size_t k = 0; k < nE; ++k) a[k] = q.eqA[k] >= 0 ? f.values[k] : 0.0;
    for (std::size_t k = 0; k < nE; ++k)
        if (q.eqA[k] == -3) a[k] = q.fixedA[k];
    const std::vector<std::array<Expression, 3>> J = sourcesOf(-1);
    std::vector<double> psi(std::size_t(ns.size()), 0.0);
    const int rounds = a_nonlinear ? std::max(2, o.maxIterations) : 1;
    bool converged = !a_nonlinear;
    // The saddle point is real, symmetric and indefinite: a real LDLᵀ with
    // pivoting (an LU of a solenoid's 53,705 unknowns took 18.6 s; the
    // complex symmetric LDLᵀ it was factorized by before, its imaginary
    // parts nought, some four times the work of the real one).
    // (One step of iterative refinement against the matrix: the pivots of a
    // saddle point scaled as it is lose a digit or two.)
    double residual = 0;
    const auto solveWith = [this, &residual](const Eigen::VectorXd& b) -> Eigen::VectorXd {
        Eigen::VectorXd x = a_lu->solve(b);
        const Eigen::VectorXd r0 = b - a_factored * x;
        x += a_lu->solve(r0);
        const double bn = b.norm();
        residual = bn > 0 ? (b - a_factored * x).norm() / bn : 0;
        return x;
    };
    for (int it = 0; it < rounds; ++it) {
        if (o.cancelled && o.cancelled()) {
            *error = tr("cancelled");
            return false;
        }
        Eigen::SparseMatrix<double> M;
        Eigen::VectorXd r;
        QElapsedTimer clock;
        clock.start();
        // (A Newton's step's said, past the first.)
        const QString step = a_nonlinear && it > 0 ? tr(", Newton's step %1").arg(it + 1) : QString();
        o.tell(tr("%1: assembling%2").arg(name, step));
        assemble(q, a, J, a_magnets, M, r, gaugeScale);
        const qint64 assembled = clock.elapsed();
        if (a_lu && a_lu->info() == Eigen::Success && sameMatrix(M, a_factored)) {
            a_s.log << tr("%1: assembled in %2 s; its matrix unchanged, its factors kept").arg(name).arg(assembled / 1000.0, 0, 'f', 2);
        } else {
            a_lu.reset();
            a_factored = Eigen::SparseMatrix<double>();
            auto lu = std::make_unique<SparseRealLdlt>();
            o.tell(tr("%1: factorizing, %2 unknowns%3").arg(name).arg(M.rows()).arg(step));
            lu->compute(M);
            // (Its analysis - the ordering, kept from a pattern factorized
            // before or made - and its numeric factorization each said, and
            // its factors' size, where the backend says them: Accelerate's
            // and MUMPS's, not an LU's.)
            const QString factorized = tr("%1: assembled in %2 s, %3 nonzeros; factorized in %4 s")
                                           .arg(name)
                                           .arg(assembled / 1000.0, 0, 'f', 2)
                                           .arg(M.nonZeros())
                                           .arg((clock.elapsed() - assembled) / 1000.0, 0, 'f', 2);
            if (lu->factorBytes() > 0)
                a_s.log << factorized
                               + tr(" (its analysis %1 s%2, its numeric factorization %3 s; its factors %4 MB)")
                                     .arg(lu->analysisSeconds(), 0, 'f', 2)
                                     .arg(lu->reusedAnalysis() ? tr(", kept from before") : QString())
                                     .arg(lu->factorSeconds(), 0, 'f', 2)
                                     .arg(lu->factorBytes() / 1048576.0, 0, 'f', 1);
            else
                a_s.log << factorized + tr(" (%1)").arg(lu->name());
            if (lu->info() != Eigen::Success) {
                *error = tr("%1: its system could not be factorized (%2) - is A held somewhere (Magnetic Insulation, a Magnetic Potential)?")
                             .arg(name, QString::fromStdString(lu->lastErrorMessage()));
                return false;
            }
            a_lu = std::move(lu);
            a_factored = std::move(M);
        }
        if (it == 0)
            a_s.log << tr("%1: %2 unknowns (%3 of A on edge elements of order %4, %5 of the gauge's multiplier), %6")
                           .arg(name)
                           .arg(q.nA + q.nP)
                           .arg(q.nA)
                           .arg(es.order())
                           .arg(q.nP)
                           .arg(a_lu->name());
        o.tell(tr("%1: solving with its factors%2").arg(name, step));
        const Eigen::VectorXd x = solveWith(r);
        if (it == 0) a_s.log << tr("%1: relative residual %2, refined once").arg(name).arg(residual, 0, 'g', 2);
        if (!x.allFinite()) {
            *error = tr("%1: its solve is not finite").arg(name);
            return false;
        }
        double num = 0, den = 0;
        for (std::size_t k = 0; k < nE; ++k)
            if (q.eqA[k] >= 0) {
                const double dv = x(q.eqA[k]);
                a[k] += dv;
                num += dv * dv;
                den += a[k] * a[k];
            }
        for (std::size_t k = 0; k < psi.size(); ++k) psi[k] = q.eqP[k] >= 0 ? x(q.nA + q.eqP[k]) / gaugeScale : 0.0;
        if (!a_nonlinear) break;
        const double change = den > 0 ? std::sqrt(num / den) : std::sqrt(num);
        a_s.log << tr("%1, Newton's step %2: relative change %3").arg(name).arg(it + 1).arg(change, 0, 'g', 3);
        if (change < o.tolerance && it > 0) {
            converged = true;
            break;
        }
    }
    if (!converged) {
        *error = tr("%1: Newton's method did not converge in %2 steps").arg(name).arg(rounds);
        return false;
    }
    f.values.assign(nE + std::size_t(ns.size()), 0.0);
    std::copy(a.begin(), a.end(), f.values.begin());
    std::copy(psi.begin(), psi.end(), f.values.begin() + std::ptrdiff_t(nE));
    f.solved = true;
    // The coils swept at 1 A each, the others off and no magnets (a linear
    // model's: its matrix's factors kept): each one's flux through every
    // other, the inductance matrix L_ij = ∫ A_j·J_i / I_i.
    a_inductance.clear();
    if (!a_coils.empty() && !a_nonlinear && o.sweep) {
        const int nc = int(a_coils.size());
        std::vector<std::vector<std::array<Expression, 3>>> unit(static_cast<std::size_t>(nc));
        for (int c = 0; c < nc; ++c) unit[std::size_t(c)] = sourcesOf(c);
        std::vector<std::vector<double>> fields(static_cast<std::size_t>(nc));
        for (int c = 0; c < nc; ++c) {
            o.tell(tr("%1: the inductance matrix, coil %2 of %3").arg(name).arg(c + 1).arg(nc));
            std::vector<double> a0(nE, 0.0);   // (held values nought)
            Eigen::SparseMatrix<double> M;
            Eigen::VectorXd r;
            assemble(q, a0, unit[std::size_t(c)], false, M, r, gaugeScale);
            const Eigen::VectorXd x = solveWith(r);
            for (std::size_t k = 0; k < nE; ++k)
                if (q.eqA[k] >= 0) a0[k] = x(q.eqA[k]);
            fields[std::size_t(c)] = std::move(a0);
        }
        a_inductance.assign(std::size_t(nc), std::vector<double>(std::size_t(nc), 0.0));
        const Mesh3& mesh = a_s.mesh();
        const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
        PointEvaluator3 ev(a_s);
        for (int e = 0; e < es.elements(); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            const int* ed = es.dofs(e);
            for (const auto& qp : rule.points) {
                EdgeSpace3::Basis b;
                es.basis(e, qp[0], qp[1], qp[2], b);
                ev.at(e, qp[0], qp[1], qp[2], b.x);
                for (int i = 0; i < nc; ++i) {
                    const auto& ji = unit[std::size_t(i)][std::size_t(d)];
                    if (!ji[0].isValid() && !ji[1].isValid() && !ji[2].isValid()) continue;
                    double jv[3];
                    for (int k = 0; k < 3; ++k) jv[k] = ji[std::size_t(k)].isValid() ? ev.eval(ji[std::size_t(k)]) : 0.0;
                    for (int j = 0; j < nc; ++j) {
                        double A[3] = {0, 0, 0};
                        for (int m = 0; m < b.count; ++m)
                            if (ed[m] >= 0)
                                for (int k = 0; k < 3; ++k) A[k] += b.v[m][k] * fields[std::size_t(j)][std::size_t(ed[m])];
                        a_inductance[std::size_t(i)][std::size_t(j)] += qp[3] * b.det * (A[0] * jv[0] + A[1] * jv[1] + A[2] * jv[2]);
                    }
                }
            }
        }
    }
    return true;
}

// --------------------------------------------------- The frequency domain

/// ν = (µ0 (µr' - j µr''))⁻¹, a 3 x 3 complex tensor (row by row): µr'' on
/// the diagonal (a core's losses, isotropic).
bool complexReluctivity(const double mur[9], double murIm, std::complex<double> nu[9])
{
    Eigen::Matrix3cd m;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m(i, j) = Mu0 * std::complex<double>(mur[3 * i + j], i == j ? -murIm : 0.0);
    Eigen::FullPivLU<Eigen::Matrix3cd> lu(m);
    if (!lu.isInvertible()) return false;
    const Eigen::Matrix3cd inv = lu.inverse();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) nu[3 * i + j] = inv(i, j);
    return true;
}

std::vector<int> MagneticProblem3::heldFacesOf(const QString& tag, QStringList* names) const
{
    const Topology3& topo = a_s.topology();
    const Model& model = a_s.model();
    const std::size_t F = topo.faces.size();
    std::vector<int> cond(F, -1);
    names->clear();
    const Node* en = model.find(tag);
    if (!en) return cond;
    for (const Node& feat : en->children) {
        if (!feat.enabled) continue;
        const NodeKind* k = feat.kind();
        if (!k || k->selection != Level::Boundary) continue;
        const QString t = feat.type;
        const bool terminal = t.endsWith(QLatin1String("/terminal")) || t.endsWith(QLatin1String("/floating"));
        const bool ground = t.endsWith(QLatin1String("/ground")) || t.endsWith(QLatin1String("/potential"));
        const bool other = t.endsWith(QLatin1String("/insulation")) || t.endsWith(QLatin1String("/zerocharge"));
        if (!terminal && !ground && !other) continue;
        QString why;
        const QVector<int> picked = resolveSelection3(topo, model, k->selection, feat.selection(), &why);
        int v = -1;
        if (ground) v = -2;
        else if (terminal) {
            *names << (t.endsWith(QLatin1String("/terminal")) ? feat.text(QStringLiteral("name")) : feat.name());
            v = int(names->size()) - 1;
        }
        for (int b : picked)
            if (b >= 0 && b < int(F)) cond[std::size_t(b)] = v;
    }
    return cond;
}

double MagneticProblem3::carriedCurrent(const Coil& c) const
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    QStringList names;
    const std::vector<int> cond = heldFacesOf(c.ec, &names);
    if (names.isEmpty()) return NaN;
    const int mine = c.terminal.isEmpty() ? 0 : int(names.indexOf(c.terminal));
    if (mine < 0) return NaN;
    std::vector<char> terminal(std::size_t(ns.size()), 0);
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi)
        if (cond[std::size_t(mesh.faces[std::size_t(fi)].face)] == mine)
            for (int dof : ns.faceDofs(fi))
                if (dof >= 0) terminal[std::size_t(dof)] = 1;
    static const char* const axes[3] = {"x", "y", "z"};
    Expression J[3];
    for (int k = 0; k < 3; ++k) J[k] = Expression::compile(QStringLiteral("%1.J%2").arg(c.ec, QString::fromLatin1(axes[k])), a_s.pointScope());
    if (!J[0].isValid() || !J[1].isValid() || !J[2].isValid()) return NaN;
    const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    PointEvaluator3 ev(a_s);
    double sum = 0;
    bool any = false;
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)] || !c.on[std::size_t(d)]) continue;
        const int* nd = ns.dofs(e);
        bool touches = false;
        for (int k = 0; k < ns.perElement() && !touches; ++k) touches = terminal[std::size_t(nd[k])];
        if (!touches) continue;
        any = true;
        const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
        const AffineMap am = affineMap(es.planNodes(), t);
        const double hz = es.z(layer + 1) - es.z(layer);
        for (const auto& qp : rule.points) {
            EdgeSpace3::Basis b;
            es.basis(e, qp[0], qp[1], qp[2], b);
            ev.at(e, qp[0], qp[1], qp[2], b.x);
            PrismShape sh;
            prismShapeAt(ns.order(), qp[0], qp[1], qp[2], sh);
            double g[18][3];
            nodalGradients(sh, am, hz, g);
            double gh[3] = {0, 0, 0};
            for (int k = 0; k < sh.count; ++k)
                if (terminal[std::size_t(nd[k])])
                    for (int m = 0; m < 3; ++m) gh[m] += g[k][m];
            double jv[3];
            for (int k = 0; k < 3; ++k) {
                jv[k] = ev.eval(J[k]);
                if (!std::isfinite(jv[k])) jv[k] = 0;
            }
            sum -= qp[3] * b.det * (jv[0] * gh[0] + jv[1] * gh[1] + jv[2] * gh[2]);
        }
    }
    return any ? sum : NaN;
}

bool MagneticProblem3::buildConductors(Conductors& h, QString* error)
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const Topology3& topo = a_s.topology();
    const std::size_t D = topo.domains.size(), F = topo.faces.size(), N = std::size_t(ns.size());
    // A single conductor's domains that have no conductivity of their own:
    // its currents physics' (the same metal).
    for (const Coil& c : a_coils) {
        if (!c.single) continue;
        const int fi = a_s.fieldIndex(c.ec);
        if (fi < 0) continue;
        const Solution3::Coefficients& ec = a_s.coefficients()[std::size_t(fi)];
        for (std::size_t d = 0; d < D && d < ec.c.size(); ++d) {
            if (!c.on[d] || !f.domains[d] || conducts(int(d))) continue;
            bool any = false;
            for (const Expression& e : ec.c[d]) any = any || (e.isValid() && !(e.isConstant() && e.constant() == 0));
            if (any) a_sigma[d] = ec.c[d];
        }
    }
    publishConductivity();
    h = Conductors();
    h.conducting.assign(D, 0);
    for (std::size_t d = 0; d < D; ++d) h.conducting[d] = f.domains[d] && conducts(int(d)) ? 1 : 0;
    // The faces each single conductor's currents physics holds - the last
    // condition on a face its own, as that physics takes them: its coil's
    // terminal (the coil), or a ground (any other it holds: -2).
    std::vector<int> faceRole(F, -1);
    QStringList tags;
    for (const Coil& c : a_coils)
        if (c.single && !tags.contains(c.ec)) tags << c.ec;
    for (const QString& tag : tags) {
        QStringList names;
        const std::vector<int> cond = heldFacesOf(tag, &names);
        for (std::size_t b = 0; b < F; ++b) {
            if (cond[b] == -1) continue;
            int role = -2;
            if (cond[b] >= 0) {
                const QString& name = names[cond[b]];
                for (int k = 0; k < int(a_coils.size()); ++k) {
                    const Coil& c = a_coils[std::size_t(k)];
                    if (c.single && c.ec == tag && (c.terminal.isEmpty() ? name == names.first() : name == c.terminal)) role = k;
                }
            }
            // (A ground's face wins over a terminal another physics holds.)
            if (faceRole[b] == -1 || role == -2) faceRole[b] = role;
        }
    }
    h.eqV.assign(N, -1);
    h.heldV.assign(N, cd(0));
    h.electrode.assign(N, 0);
    h.terminalOf.assign(N, -1);
    for (int e = 0; e < ns.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !h.conducting[std::size_t(d)]) continue;
        const int* nd = ns.dofs(e);
        for (int k = 0; k < ns.perElement(); ++k) h.eqV[std::size_t(nd[k])] = -2;
    }
    std::vector<char> ground(N, 0);
    h.electrodeFaces.assign(mesh.faces.size(), 0);
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
        const int role = faceRole[std::size_t(mesh.faces[std::size_t(fi)].face)];
        if (role == -1) continue;
        h.electrodeFaces[std::size_t(fi)] = 1;
        for (int dof : ns.faceDofs(fi)) {
            if (dof < 0 || h.eqV[std::size_t(dof)] == -1) continue;
            h.electrode[std::size_t(dof)] = 1;
            if (role >= 0) h.terminalOf[std::size_t(dof)] = role;
            else ground[std::size_t(dof)] = 1;
        }
    }
    for (std::size_t k = 0; k < N; ++k)
        if (ground[k]) h.terminalOf[k] = -1;
    // Each conductor (the nodes its elements join): one that touches no
    // terminal or ground has its potential's constant held at one node;
    // a coil's must touch a ground (its current's way back).
    std::vector<int> parent(N);
    for (std::size_t k = 0; k < N; ++k) parent[k] = int(k);
    const auto root = [&parent](int k) {
        while (parent[std::size_t(k)] != k) {
            parent[std::size_t(k)] = parent[std::size_t(parent[std::size_t(k)])];
            k = parent[std::size_t(k)];
        }
        return k;
    };
    for (int e = 0; e < ns.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !h.conducting[std::size_t(d)]) continue;
        const int* nd = ns.dofs(e);
        const int r0 = root(nd[0]);
        for (int k = 1; k < ns.perElement(); ++k) {
            const int rk = root(nd[k]);
            if (rk != r0) parent[std::size_t(rk)] = r0;
        }
    }
    std::vector<char> grounded(N, 0), touched(N, 0);
    for (std::size_t k = 0; k < N; ++k) {
        if (h.eqV[k] == -1) continue;
        const int r = root(int(k));
        if (ground[k]) grounded[std::size_t(r)] = 1;
        if (h.electrode[k]) touched[std::size_t(r)] = 1;
    }
    for (int c = 0; c < int(a_coils.size()); ++c) {
        const Coil& coil = a_coils[std::size_t(c)];
        if (!coil.single) continue;
        int at = -1;
        for (std::size_t k = 0; k < N && at < 0; ++k)
            if (h.terminalOf[k] == c) at = int(k);
        if (at < 0) {
            *error = tr("coil %1: in the frequency domain a single conductor is fed at its currents physics' terminal (%2 of %3) - none is on a "
                        "domain of %4 that conducts (its material's sigma, Ampère's Law's or %3's)")
                         .arg(coil.name, coil.terminal.isEmpty() ? tr("its first") : coil.terminal, coil.ec, a_node->name());
            return false;
        }
        if (!grounded[std::size_t(root(at))]) {
            *error = tr("coil %1: its conductor touches no ground of its currents physics %2 (a Ground on it): in the frequency domain its current "
                        "would have no way back")
                         .arg(coil.name, coil.ec);
            return false;
        }
    }
    for (std::size_t k = 0; k < N; ++k) {
        if (h.eqV[k] == -1) continue;
        const int r = root(int(k));
        if (!touched[std::size_t(r)]) {
            touched[std::size_t(r)] = 1;   // (its first node held, the rest free)
            h.eqV[k] = -3;
        }
    }
    // The equations: each coil's terminal one (its current held), or held
    // at its voltage; a ground's nought; the rest one each.
    h.eqU.assign(a_coils.size(), -1);
    int next = 0;
    for (int c = 0; c < int(a_coils.size()); ++c) {
        const Coil& coil = a_coils[std::size_t(c)];
        if (coil.single && (coil.role == Coil::Current || coil.role == Coil::Floating)) h.eqU[std::size_t(c)] = next++;
    }
    for (std::size_t k = 0; k < N; ++k) {
        if (h.eqV[k] != -2) continue;
        if (!h.electrode[k]) {
            h.eqV[k] = next++;
            continue;
        }
        const int c = h.terminalOf[k];
        h.eqV[k] = c >= 0 && h.eqU[std::size_t(c)] >= 0 ? h.eqU[std::size_t(c)] : -3;
        if (c >= 0 && a_coils[std::size_t(c)].role == Coil::Voltage) h.heldV[k] = evalAt(a_coils[std::size_t(c)].voltage, Vec3{});
    }
    h.nV = next;
    return true;
}

void MagneticProblem3::assembleHarmonic(const Equations& q, const Conductors& h, int parts, const std::vector<double>& held,
                                        const std::vector<cd>& heldV, const std::vector<std::array<Expression, 3>>& J, bool matrix,
                                        bool sheets, Eigen::SparseMatrix<cd>& M, Eigen::VectorXcd& r, double gaugeScale) const
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const int oP = q.nA, oV = q.nA + q.nP, n = oV + h.nV;
    // (K1's σ blocks of jω 1; K0's none. K0's curl-curl, gauge and sources
    // alone in it.)
    const bool k0 = parts == 1;
    const cd jw = k0 ? cd(0) : cd(1);
    const double curl = k0 ? 1.0 : 0.0;
    using T = Eigen::Triplet<cd>;
    std::vector<T> tm;
    r = Eigen::VectorXcd::Zero(n);
    const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    const int nb = es.perElement(), npn = ns.perElement();
    if (matrix) tm.reserve(std::size_t(es.elements()) * std::size_t(nb * nb + 2 * nb * npn));
    PointEvaluator3 ev(a_s);
    std::vector<cd> Ke(static_cast<std::size_t>(nb * nb)), Cav(static_cast<std::size_t>(nb * npn)), Lvv(static_cast<std::size_t>(npn * npn)), Fe(static_cast<std::size_t>(nb));
    std::vector<double> Be(static_cast<std::size_t>(npn * nb));
    const std::vector<Expression>& murIm = coef.named.at(QStringLiteral("murIm"));
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        const std::array<Expression, 9>& mur = coef.c[std::size_t(d)];
        const Expression& im = murIm[std::size_t(d)];
        bool constNu = !im.isValid() || im.isConstant();
        for (const Expression& x : mur) constNu = constNu && (!x.isValid() || x.isConstant());
        const bool conducting = h.conducting[std::size_t(d)];
        const std::array<Expression, 9>& sg = a_sigma[std::size_t(d)];
        bool constSigma = true;
        for (const Expression& x : sg) constSigma = constSigma && (!x.isValid() || x.isConstant());
        cd nu[9];
        double s9[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        const auto readNu = [&](bool at) {
            double m9[9];
            for (int k = 0; k < 9; ++k) m9[k] = mur[std::size_t(k)].isValid() ? (at ? ev.eval(mur[std::size_t(k)]) : mur[std::size_t(k)].constant()) : 0;
            const double mi = im.isValid() ? (at ? ev.eval(im) : im.constant()) : 0.0;
            complexReluctivity(m9, std::isfinite(mi) ? mi : 0.0, nu);
        };
        const auto readSigma = [&](bool at) {
            for (int k = 0; k < 9; ++k) {
                s9[k] = sg[std::size_t(k)].isValid() ? (at ? ev.eval(sg[std::size_t(k)]) : sg[std::size_t(k)].constant()) : 0;
                if (!std::isfinite(s9[k])) s9[k] = 0;
            }
        };
        if (constNu) readNu(false);
        // (A B-H curve's element: its effective µr at each of its points.)
        const std::size_t nqe = rule.points.size();
        const bool effective = !a_muEff.empty() && a_muEff.size() == mesh.prisms.size() * nqe && d < int(coef.curves.size()) && coef.curves[std::size_t(d)]
                               && a_muEff[std::size_t(e) * nqe] > 0;
        int qi = -1;
        if (conducting && constSigma) readSigma(false);
        const bool sourced = k0 && (J[std::size_t(d)][0].isValid() || J[std::size_t(d)][1].isValid() || J[std::size_t(d)][2].isValid());
        std::fill(Ke.begin(), Ke.end(), cd(0));
        std::fill(Cav.begin(), Cav.end(), cd(0));
        std::fill(Lvv.begin(), Lvv.end(), cd(0));
        std::fill(Fe.begin(), Fe.end(), cd(0));
        std::fill(Be.begin(), Be.end(), 0.0);
        const int* ed = es.dofs(e);
        const int* nd = ns.dofs(e);
        const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
        const AffineMap am = affineMap(es.planNodes(), t);
        const double hz = es.z(layer + 1) - es.z(layer);
        for (const auto& qp : rule.points) {
            ++qi;
            EdgeSpace3::Basis b;
            es.basis(e, qp[0], qp[1], qp[2], b);
            const double w = qp[3] * b.det;
            if (!constNu || (conducting && !constSigma) || sourced) ev.at(e, qp[0], qp[1], qp[2], b.x);
            if (!constNu && !effective) readNu(true);
            if (effective) {
                for (cd& v : nu) v = 0;
                nu[0] = nu[4] = nu[8] = 1 / (Mu0 * a_muEff[std::size_t(e) * nqe + std::size_t(qi)]);
            }
            if (conducting && !constSigma) readSigma(true);
            PrismShape s;
            prismShapeAt(ns.order(), qp[0], qp[1], qp[2], s);
            double g[18][3];
            nodalGradients(s, am, hz, g);
            double Jv[3] = {0, 0, 0};
            if (sourced)
                for (int k = 0; k < 3; ++k)
                    if (J[std::size_t(d)][std::size_t(k)].isValid()) {
                        Jv[k] = ev.eval(J[std::size_t(d)][std::size_t(k)]);
                        if (!std::isfinite(Jv[k])) Jv[k] = 0;
                    }
            // (Also for a right-hand side alone: what the held values give.)
            for (int j = 0; j < nb; ++j) {
                // ν curl Wj, σ Wj
                cd nc[3];
                for (int k = 0; k < 3; ++k) nc[k] = nu[3 * k] * b.c[j][0] + nu[3 * k + 1] * b.c[j][1] + nu[3 * k + 2] * b.c[j][2];
                double sw[3] = {0, 0, 0};
                if (conducting)
                    for (int k = 0; k < 3; ++k) sw[k] = s9[3 * k] * b.v[j][0] + s9[3 * k + 1] * b.v[j][1] + s9[3 * k + 2] * b.v[j][2];
                for (int i = 0; i < nb; ++i) {
                    cd v = curl * (b.c[i][0] * nc[0] + b.c[i][1] * nc[1] + b.c[i][2] * nc[2]);
                    if (conducting) v += jw * (b.v[i][0] * sw[0] + b.v[i][1] * sw[1] + b.v[i][2] * sw[2]);
                    Ke[std::size_t(i * nb + j)] += w * v;
                }
            }
            if (conducting)
                for (int k = 0; k < npn; ++k) {
                    double sgk[3];
                    for (int c = 0; c < 3; ++c) sgk[c] = s9[3 * c] * g[k][0] + s9[3 * c + 1] * g[k][1] + s9[3 * c + 2] * g[k][2];
                    for (int i = 0; i < nb; ++i) Cav[std::size_t(i * npn + k)] += w * jw * (b.v[i][0] * sgk[0] + b.v[i][1] * sgk[1] + b.v[i][2] * sgk[2]);
                    for (int l = 0; l < npn; ++l) Lvv[std::size_t(l * npn + k)] += w * jw * (g[l][0] * sgk[0] + g[l][1] * sgk[1] + g[l][2] * sgk[2]);
                }
            for (int k = 0; k < npn; ++k)
                for (int j = 0; j < nb; ++j) Be[std::size_t(k * nb + j)] += curl * w * gaugeScale * (g[k][0] * b.v[j][0] + g[k][1] * b.v[j][1] + g[k][2] * b.v[j][2]);
            if (sourced)
                for (int i = 0; i < nb; ++i) Fe[std::size_t(i)] += w * (Jv[0] * b.v[i][0] + Jv[1] * b.v[i][1] + Jv[2] * b.v[i][2]);
        }
        const auto vEq = [&](int k) { return h.eqV[std::size_t(nd[k])]; };
        for (int i = 0; i < nb; ++i) {
            const int ei = ed[i] >= 0 ? q.eqA[std::size_t(ed[i])] : -1;
            if (ei < 0) continue;
            r(ei) += Fe[std::size_t(i)];
            for (int j = 0; j < nb; ++j) {
                const int ej = ed[j] >= 0 ? q.eqA[std::size_t(ed[j])] : -1;
                const cd v = Ke[std::size_t(i * nb + j)];
                if (ej >= 0) {
                    if (matrix) tm.emplace_back(ei, ej, v);
                } else if (ed[j] >= 0 && ej == -3 && held[std::size_t(ed[j])] != 0) {
                    r(ei) -= v * held[std::size_t(ed[j])];
                }
            }
            if (conducting)
                for (int k = 0; k < npn; ++k) {
                    const int vk = vEq(k);
                    const cd v = Cav[std::size_t(i * npn + k)];
                    if (vk >= 0) {
                        if (matrix) tm.emplace_back(ei, oV + vk, v);
                    } else if (vk == -3 && heldV[std::size_t(nd[k])] != cd(0)) {
                        r(ei) -= v * heldV[std::size_t(nd[k])];
                    }
                }
        }
        for (int k = 0; k < npn; ++k) {
            const int pk = q.eqP[std::size_t(nd[k])];
            if (pk < 0) continue;
            for (int j = 0; j < nb; ++j) {
                const int ej = ed[j] >= 0 ? q.eqA[std::size_t(ed[j])] : -1;
                const double v = Be[std::size_t(k * nb + j)];
                if (ej >= 0) {
                    if (matrix) {
                        tm.emplace_back(oP + pk, ej, v);
                        tm.emplace_back(ej, oP + pk, v);
                    }
                } else if (ed[j] >= 0 && ej == -3 && held[std::size_t(ed[j])] != 0) {
                    r(oP + pk) -= v * held[std::size_t(ed[j])];
                }
            }
        }
        if (!conducting) continue;
        for (int k = 0; k < npn; ++k) {
            const int vk = vEq(k);
            if (vk < 0) continue;
            for (int j = 0; j < nb; ++j) {
                const int ej = ed[j] >= 0 ? q.eqA[std::size_t(ed[j])] : -1;
                const cd v = Cav[std::size_t(j * npn + k)];
                if (ej >= 0) {
                    if (matrix) tm.emplace_back(oV + vk, ej, v);
                } else if (ed[j] >= 0 && ej == -3 && held[std::size_t(ed[j])] != 0) {
                    r(oV + vk) -= v * held[std::size_t(ed[j])];
                }
            }
            for (int l = 0; l < npn; ++l) {
                const int vl = vEq(l);
                const cd v = Lvv[std::size_t(l * npn + k)];
                if (vl >= 0) {
                    if (matrix) tm.emplace_back(oV + vk, oV + vl, v);
                } else if (vl == -3 && heldV[std::size_t(nd[l])] != cd(0)) {
                    r(oV + vk) -= v * heldV[std::size_t(nd[l])];
                }
            }
        }
    }
    // The surface currents: ∫ Js·Wi on their faces (the sweep's: none).
    if (sheets && k0) {
        const int degree = es.order() == 1 ? 2 : 4;
        for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
            const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];
            const auto& sheets = a_sheets[std::size_t(mf.face)];
            if (sheets.empty()) continue;
            const Topology3::Face& tf = a_s.topology().faces[std::size_t(mf.face)];
            const int side = tf.back >= 0 && f.domains[std::size_t(tf.back)] ? tf.back : tf.front;
            for (const Space3::FacePoint& fp : ns.facePoints(fi, degree, side)) {
                if (fp.prism < 0) continue;
                EdgeSpace3::Basis b;
                es.basis(fp.prism, fp.xi, fp.eta, fp.zeta, b);
                ev.at(fp.prism, fp.xi, fp.eta, fp.zeta, fp.x);
                double js[3] = {0, 0, 0};
                for (const auto& sh : sheets)
                    for (int k = 0; k < 3; ++k) {
                        const double v = ev.eval(sh[std::size_t(k)]);
                        if (std::isfinite(v)) js[k] += v;
                    }
                const int* ed = es.dofs(fp.prism);
                for (int i = 0; i < b.count; ++i) {
                    const int ei = ed[i] >= 0 ? q.eqA[std::size_t(ed[i])] : -1;
                    if (ei >= 0) r(ei) += fp.weight * (js[0] * b.v[i][0] + js[1] * b.v[i][1] + js[2] * b.v[i][2]);
                }
            }
        }
    }
    if (matrix) {
        M.resize(n, n);
        M.setFromTriplets(tm.begin(), tm.end());
    }
}

void MagneticProblem3::skinDepthCheck(const Conductors& h, double omega)
{
    // The skin depth δ = √(2/(ωµσ)) against the mesh at each conductor's
    // surface - a face between it and what does not conduct -, its prism's
    // size across the face (a wall's: its triangle's height over that edge;
    // a floor's: its layer's): past δ (order 2; δ/2 at order 1) the eddy
    // currents are not resolved there, and the swept mesh does not make
    // itself finer as a 2D mesh adapting would.
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const Topology3& topo = a_s.topology();
    const Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const auto conducting = [&h](int d) { return d >= 0 && d < int(h.conducting.size()) && h.conducting[std::size_t(d)]; };
    PointEvaluator3 ev(a_s);
    double worst = 0, sizeAt = 0, deltaAt = 0;
    int domainAt = -1;
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
        const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];
        const Topology3::Face& tf = topo.faces[std::size_t(mf.face)];
        if (conducting(tf.back) == conducting(tf.front)) continue;
        const int side = conducting(tf.back) ? tf.back : tf.front;
        const std::vector<Space3::FacePoint> points = ns.facePoints(fi, 1, side);
        if (points.empty() || points.front().prism < 0) continue;
        const int e = points.front().prism;
        if (mesh.domain[std::size_t(e)] != side) continue;
        const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
        double size = es.z(layer + 1) - es.z(layer);
        if (mf.quad) {
            const Mesh::Edge& pe = mesh.plan->edges[std::size_t(mf.plan)];
            const QPointF d = (mesh.plan->nodes[std::size_t(pe.a)] - mesh.plan->nodes[std::size_t(pe.b)]) * mesh.unitScale;
            const double len = std::hypot(d.x(), d.y());
            if (!(len > 0)) continue;
            size = std::abs(affineMap(es.planNodes(), t).det) / len;
        }
        EdgeSpace3::Basis b;
        es.basis(e, 1.0 / 3, 1.0 / 3, 0.5, b);
        ev.at(e, 1.0 / 3, 1.0 / 3, 0.5, b.x);
        double sigma = 0, mur = 0;
        for (int k : {0, 4, 8}) {
            const Expression& sg = a_sigma[std::size_t(side)][std::size_t(k)];
            const Expression& mu = coef.c[std::size_t(side)][std::size_t(k)];
            if (sg.isValid()) sigma += ev.eval(sg) / 3;
            mur += (mu.isValid() ? ev.eval(mu) : 1.0) / 3;
        }
        if (!(sigma > 0) || !(mur > 0)) continue;
        const double delta = std::sqrt(2 / (omega * Mu0 * mur * sigma));
        if (size / delta > worst) {
            worst = size / delta;
            sizeAt = size;
            deltaAt = delta;
            domainAt = side;
        }
    }
    if (worst > (es.order() == 2 ? 1.0 : 0.5))
        a_s.log << tr("Warning: %1 at %2: the mesh at the surface of domain %3 (%4) is %5 across where its skin depth is %6 - its eddy currents "
                      "are not resolved: make the mesh finer there (a Size on it, or the layers' divisions)")
                       .arg(a_node->name(), formatQuantity(omega / (2 * M_PI), Dim::of(0, 0, -1)))
                       .arg(domainAt + 1)
                       .arg(topo.objectNames.value(topo.domains[std::size_t(domainAt)].owner), formatQuantity(sizeAt, Dim::length()),
                            formatQuantity(deltaAt, Dim::length()));
}

double MagneticProblem3::gaugeScaleOf() const
{
    // The gauge's rows scaled to the curl-curl's: ν0 / h², h the mesh's
    // typical prism (the multiplier is nought where J is free of divergence:
    // only the pivots mind).
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const EdgeSpace3& es = *f.edges;
    const Mesh3& mesh = a_s.mesh();
    double volume = 0;
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        EdgeSpace3::Basis b;
        es.basis(e, 1.0 / 3, 1.0 / 3, 0.5, b);
        volume += 0.5 * b.det;
    }
    const double hTypical = std::cbrt(std::max(volume, 1e-300) / std::max(1, es.elements()));
    return 1 / (Mu0 * hTypical * hTypical);
}

bool MagneticProblem3::prepareHarmonic(const Options3& o, Harmonic& hs, QString* error)
{
    Field3& f = field();
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const QString name = a_node->name();
    if (a_nonlinear && a_muEff.empty()) {
        *error = tr("%1: a B-H curve in the frequency domain is taken by its effective permeability, solved in turn").arg(name);
        return false;
    }
    a_inductance.clear();
    a_harmonic = true;
    if (!buildSources(error)) return false;
    Conductors& h = hs.h;
    Equations& q = hs.q;
    if (!buildConductors(h, error)) return false;
    if (!buildEquations(q, error)) return false;
    // A coil's terminal and ground on faces whose tangential A is held
    // (Magnetic Insulation, a Magnetic Potential): n × E = 0 on them, the
    // voltage between them the field's integral through that perfect
    // conductor. Elsewhere it is not one value - A + ∇h, U - 1 the same
    // field: the system singular.
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
        if (!h.electrodeFaces[std::size_t(fi)]) continue;
        bool held = true;
        for (int dof : ns.faceDofs(fi)) held = held && (dof < 0 || q.eqP[std::size_t(dof)] == -3);
        if (held) continue;
        const int face = mesh.faces[std::size_t(fi)].face;
        *error = tr("%1: face %2 is a coil's terminal or ground (its currents physics holds it) where A is free - in the frequency domain a "
                    "conductor is fed on faces Magnetic Insulation holds, the model's outer faces (a perfect conductor closing its circuit)")
                     .arg(name)
                     .arg(face + 1);
        return false;
    }
    hs.gaugeScale = gaugeScaleOf();
    const std::size_t nE = std::size_t(es.size()), nN = std::size_t(ns.size());
    hs.held.assign(nE, 0.0);
    for (std::size_t k = 0; k < nE; ++k)
        if (q.eqA[k] == -3) hs.held[k] = q.fixedA[k];
    hs.oV = q.nA + q.nP;
    hs.n = hs.oV + h.nV;
    const int nc = int(a_coils.size());
    hs.unit.assign(static_cast<std::size_t>(nc), {});
    for (int c = 0; c < nc; ++c)
        if (!a_coils[std::size_t(c)].single) hs.unit[std::size_t(c)] = sourcesOf(c);
    // K0 and the sources less what the held A gives with it; K1 and what
    // the held A and the held potentials give with it - b(ω) = (r0 + rV +
    // the currents held) + jω rA.
    o.tell(tr("%1: assembling").arg(name));
    QElapsedTimer clock;
    clock.start();
    const std::vector<std::array<Expression, 3>> J = sourcesOf(-1);
    const std::vector<double> none(nE, 0.0);
    const std::vector<cd> noneV(nN, cd(0));
    Eigen::VectorXcd r0, rA, rV;
    assembleHarmonic(q, h, 1, hs.held, noneV, J, true, true, hs.K0, r0, hs.gaugeScale);
    assembleHarmonic(q, h, 2, hs.held, noneV, J, true, false, hs.K1, rA, hs.gaugeScale);
    Eigen::SparseMatrix<cd> unused;
    assembleHarmonic(q, h, 2, none, h.heldV, J, false, false, unused, rV, hs.gaugeScale);
    hs.bc = r0 + rV;
    for (int c = 0; c < nc; ++c)
        if (h.eqU[std::size_t(c)] >= 0 && a_coils[std::size_t(c)].role == Coil::Current) hs.bc(hs.oV + h.eqU[std::size_t(c)]) += evalAt(a_coils[std::size_t(c)].current, Vec3{});
    hs.bj = rA;
    // Each port's right-hand side, 1 A in it (ω's none): a conductor's on
    // its terminal's equation, turns' their current density.
    hs.ports.clear();
    for (int c = 0; c < nc; ++c) {
        const Coil& coil = a_coils[std::size_t(c)];
        if (coil.single && coil.role != Coil::Current) continue;
        hs.ports.push_back(c);
    }
    hs.P = Eigen::MatrixXcd::Zero(hs.n, o.sweep ? Eigen::Index(hs.ports.size()) : 0);
    if (o.sweep)
        for (int jp = 0; jp < int(hs.ports.size()); ++jp) {
            const int c = hs.ports[std::size_t(jp)];
            if (a_coils[std::size_t(c)].single) hs.P(hs.oV + h.eqU[std::size_t(c)], jp) = 1.0;
            else {
                Eigen::VectorXcd rj;
                assembleHarmonic(q, h, 1, none, noneV, hs.unit[std::size_t(c)], false, false, unused, rj, hs.gaugeScale);
                hs.P.col(jp) = rj;
            }
        }
    hs.assembled = clock.elapsed() / 1000.0;
    return true;
}

void MagneticProblem3::buildSpaces(const Harmonic& hs) const
{
    auto sp = std::make_shared<Harmonic::Spaces>();
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const Equations& q = hs.q;
    const PrismRule& rule = prismRule(4, 3);
    const int nb = es.perElement(), npn = ns.perElement();
    using T = Eigen::Triplet<double>;
    std::vector<T> tm, tg, tp, cg, cp;
    tm.reserve(std::size_t(es.elements()) * std::size_t(nb * nb));
    PointEvaluator3 ev(a_s);
    // (Π of the vertices' linear fields alone - in the edge space of
    // either order, so that each element's projection is the field's own:
    // order 2's quadratic nodal fields are not, their elements' projections
    // differing, and the cycle took some eight times the iterations.)
    static const int vertices2[6] = {0, 1, 2, 12, 13, 14};
    const int* vertexOf = ns.order() == 2 ? vertices2 : nullptr;
    std::vector<int> vertexColumn(std::size_t(ns.size()), -1);
    int nVertices = 0;
    Eigen::MatrixXd Me(nb, nb), Rg(nb, npn), Rp(nb, 3 * 6), Mn(npn, npn), Rn(npn, 6);
    std::vector<T> tv, cv;
    sp->sigma.assign(mesh.prisms.size(), 0.0);
    double volume = 0;
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        Me.setZero();
        Rg.setZero();
        Rp.setZero();
        Mn.setZero();
        Rn.setZero();
        const int* ed = es.dofs(e);
        const int* nd = ns.dofs(e);
        const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
        const AffineMap am = affineMap(es.planNodes(), t);
        const double hz = es.z(layer + 1) - es.z(layer);
        for (const auto& qp : rule.points) {
            EdgeSpace3::Basis b;
            es.basis(e, qp[0], qp[1], qp[2], b);
            const double w = qp[3] * b.det;
            volume += w;
            PrismShape s, s1;
            prismShapeAt(ns.order(), qp[0], qp[1], qp[2], s);
            prismShapeAt(1, qp[0], qp[1], qp[2], s1);
            double g[18][3];
            nodalGradients(s, am, hz, g);
            for (int i = 0; i < nb; ++i) {
                for (int j = 0; j < nb; ++j) Me(i, j) += w * (b.v[i][0] * b.v[j][0] + b.v[i][1] * b.v[j][1] + b.v[i][2] * b.v[j][2]);
                for (int k = 0; k < npn; ++k) Rg(i, k) += w * (b.v[i][0] * g[k][0] + b.v[i][1] * g[k][1] + b.v[i][2] * g[k][2]);
                for (int k = 0; k < 6; ++k)
                    for (int c = 0; c < 3; ++c) Rp(i, 3 * k + c) += w * b.v[i][c] * s1.n[k];
            }
            if (vertexOf)
                for (int k = 0; k < npn; ++k) {
                    for (int l = 0; l < npn; ++l) Mn(k, l) += w * s.n[k] * s.n[l];
                    for (int m = 0; m < 6; ++m) Rn(k, m) += w * s.n[k] * s1.n[m];
                }
        }
        // The element's own: a gradient (a nodal field) as its edge
        // functions, by its mass - exact for a gradient, which they hold; an
        // edge shared, the elements' mean.
        const Eigen::LDLT<Eigen::MatrixXd> ldlt(Me);
        const Eigen::MatrixXd Cg = ldlt.solve(Rg), Cp = ldlt.solve(Rp);
        const double small = 1e-10 * std::max(Cg.cwiseAbs().maxCoeff(), Cp.cwiseAbs().maxCoeff());
        // (Order 2: the linear nodes' fields in its own, each node's value -
        // exact, they are in it.)
        if (vertexOf) {
            const Eigen::MatrixXd Cn = Mn.ldlt().solve(Rn);
            for (int k = 0; k < npn; ++k)
                for (int m = 0; m < 6; ++m)
                    if (std::abs(Cn(k, m)) > 1e-10 && nd[k] >= 0 && nd[vertexOf[m]] >= 0) {
                        tv.emplace_back(nd[k], nd[vertexOf[m]], Cn(k, m));
                        cv.emplace_back(nd[k], nd[vertexOf[m]], 1.0);
                    }
        }
        for (int i = 0; i < nb; ++i) {
            const int ei = ed[i] >= 0 ? q.eqA[std::size_t(ed[i])] : -1;
            if (ei < 0) continue;
            for (int j = 0; j < nb; ++j) {
                const int ej = ed[j] >= 0 ? q.eqA[std::size_t(ed[j])] : -1;
                if (ej >= 0) tm.emplace_back(ei, ej, Me(i, j));
            }
            for (int k = 0; k < npn; ++k)
                if (nd[k] >= 0 && std::abs(Cg(i, k)) > small) {
                    tg.emplace_back(ei, nd[k], Cg(i, k));
                    cg.emplace_back(ei, nd[k], 1.0);
                }
            for (int k = 0; k < 6; ++k) {
                const int node = nd[vertexOf ? vertexOf[k] : k];
                if (node < 0 || q.eqP[std::size_t(node)] < 0) continue;
                int& col = vertexColumn[std::size_t(node)];
                if (col < 0) col = nVertices++;
                for (int c = 0; c < 3; ++c)
                    if (std::abs(Cp(i, 3 * k + c)) > small) {
                        tp.emplace_back(ei, 3 * col + c, Cp(i, 3 * k + c));
                        cp.emplace_back(ei, 3 * col + c, 1.0);
                    }
            }
        }
        if (hs.h.conducting[std::size_t(d)]) {
            EdgeSpace3::Basis b;
            es.basis(e, 1.0 / 3, 1.0 / 3, 0.5, b);
            ev.at(e, 1.0 / 3, 1.0 / 3, 0.5, b.x);
            const std::array<Expression, 9>& sg = a_sigma[std::size_t(d)];
            double tr3 = 0;
            for (int k : {0, 4, 8})
                if (sg[std::size_t(k)].isValid()) {
                    const double v = ev.eval(sg[std::size_t(k)]);
                    if (std::isfinite(v)) tr3 += std::abs(v) / 3;
                }
            sp->sigma[std::size_t(e)] = tr3;
        }
    }
    const auto mean = [](int rows, int cols, const std::vector<T>& v, const std::vector<T>& count) {
        Eigen::SparseMatrix<double> m(rows, cols), c(rows, cols);
        m.setFromTriplets(v.begin(), v.end());
        c.setFromTriplets(count.begin(), count.end());
        m.makeCompressed();
        c.makeCompressed();
        for (Eigen::Index k = 0; k < m.nonZeros(); ++k) m.valuePtr()[k] /= c.valuePtr()[k];
        return m;
    };
    sp->M.resize(q.nA, q.nA);
    sp->M.setFromTriplets(tm.begin(), tm.end());
    sp->Gn = mean(q.nA, ns.size(), tg, cg);
    sp->Pi = mean(q.nA, 3 * nVertices, tp, cp);
    if (vertexOf) sp->Vn = mean(ns.size(), ns.size(), tv, cv);
    // γ: the mass beside the curl-curl, about its least eigenvalue (ν0 π² /
    // L², L the model's size): 10 ν0 / L². Its choice is free - the
    // multiplier's Schur complement taken as its own (Greif and Schötzau,
    // Numer. Linear Algebra Appl. 14 (2007) 281: the gradients' block BᵀL⁻¹B,
    // M on them) - but GMRES minds it: on a coax at 10 kHz (each block solved
    // exactly) 225 iterations at 1e-6 ν0 / L², 100 at 1e-2, 40 to 46 from 10
    // to 100, 100 at 1000; at 1 MHz (the cycles) 157 at 3, 164 at 10, 219 at
    // 100.
    const double size = std::cbrt(std::max(volume, 1e-300));
    sp->gamma = 10 / (Mu0 * size * size);
    hs.spaces = sp;
}

bool MagneticProblem3::solveIteratively(const Options3& o, const Harmonic& hs, double omega, Eigen::MatrixXcd& X, QString* error, int moments)
{
    const QString name = a_node->name();
    const QString at = tr(" at %1").arg(formatQuantity(omega / (2 * M_PI), Dim::of(0, 0, -1)));
    const cd jw(0, omega);
    QElapsedTimer clock;
    clock.start();
    o.tell(tr("%1: setting the iterative solve up, %2 unknowns").arg(name).arg(hs.n));
    if (!hs.spaces) buildSpaces(hs);
    const Harmonic::Spaces& sp = *hs.spaces;
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const Equations& q = hs.q;
    const int nA = q.nA, nP = q.nP, nV = hs.n - hs.oV;
    // The preconditioner, real, its matrix |K0 + jω K1|'s: Re K0 + ω K1 on A
    // and the conductors' potential together, γ M added to A's - positive
    // definite (A = -∇φ, ψ = φ in a conductor, nought in K0 + ω K1, costs γ
    // φᵀLφ) -; and the gauge's multiplier's Schur complement, B P⁻¹ Bᵀ,
    // its gradients' block γ M everywhere (in a conductor their ω σ M taken
    // back by ψ): s² L / γ, s the gauge's scale (Greif and Schötzau, Numer.
    // Linear Algebra Appl. 14 (2007) 281). Their blocks apart - A's, ψ's -
    // the conductors' A and ψ together are singular where ω σ is all: at 1
    // MHz GMRES did not converge even with each block solved exactly.
    const int nS = nA + nV;
    using T = Eigen::Triplet<double>;
    Eigen::SparseMatrix<double> PS(nS, nS);
    {
        const Eigen::SparseMatrix<double> R = Eigen::SparseMatrix<double>(hs.K0.real()) + omega * Eigen::SparseMatrix<double>(hs.K1.real());
        const auto index = [&](int k) { return k < nA ? k : k >= hs.oV ? nA + (k - hs.oV) : -1; };
        std::vector<T> t;
        t.reserve(std::size_t(R.nonZeros() + sp.M.nonZeros()));
        for (int c = 0; c < R.outerSize(); ++c) {
            const int jc = index(c);
            if (jc < 0) continue;
            for (Eigen::SparseMatrix<double>::InnerIterator it(R, c); it; ++it) {
                const int ir = index(int(it.row()));
                if (ir >= 0) t.emplace_back(ir, jc, it.value());
            }
        }
        for (int c = 0; c < sp.M.outerSize(); ++c)
            for (Eigen::SparseMatrix<double>::InnerIterator it(sp.M, c); it; ++it) t.emplace_back(int(it.row()), c, sp.gamma * it.value());
        PS.setFromTriplets(t.begin(), t.end());
    }
    Eigen::SparseMatrix<double> PP(nP, nP);
    {
        std::vector<T> tl;
        const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
        const int npn = ns.perElement();
        std::vector<double> Le(std::size_t(npn * npn));
        const double weight = hs.gaugeScale * hs.gaugeScale / sp.gamma;
        for (int e = 0; e < es.elements(); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            std::fill(Le.begin(), Le.end(), 0.0);
            const int* nd = ns.dofs(e);
            const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
            const AffineMap am = affineMap(es.planNodes(), t);
            const double hz = es.z(layer + 1) - es.z(layer);
            for (const auto& qp : rule.points) {
                EdgeSpace3::Basis b;
                es.basis(e, qp[0], qp[1], qp[2], b);
                const double w = qp[3] * b.det * weight;
                PrismShape s;
                prismShapeAt(ns.order(), qp[0], qp[1], qp[2], s);
                double g[18][3];
                nodalGradients(s, am, hz, g);
                for (int k = 0; k < npn; ++k)
                    for (int l = 0; l < npn; ++l) Le[std::size_t(k * npn + l)] += w * (g[k][0] * g[l][0] + g[k][1] * g[l][1] + g[k][2] * g[l][2]);
            }
            for (int k = 0; k < npn; ++k) {
                const int pk = nd[k] >= 0 ? q.eqP[std::size_t(nd[k])] : -1;
                if (pk < 0) continue;
                for (int l = 0; l < npn; ++l) {
                    const int pl = nd[l] >= 0 ? q.eqP[std::size_t(nd[l])] : -1;
                    if (pl >= 0) tl.emplace_back(pk, pl, Le[std::size_t(k * npn + l)]);
                }
            }
        }
        PP.setFromTriplets(tl.begin(), tl.end());
    }
    // The nodal spaces' cycles: multigrid; of order 2 a two-level
    // p-multigrid, its linear nodes' multigrid below Gauss-Seidel.
    const auto nodal = [&](const std::vector<int>& eq, int n) -> CycleMaker {
        if (ns.order() == 1) return multigrid();
        const Eigen::SparseMatrix<double> V = byEquation(sp.Vn, eq, n);
        return [V](const Eigen::SparseMatrix<double>& a, QString* e) -> std::unique_ptr<Cycle> {
            auto c = std::make_unique<AuxiliaryCycle>();
            // (Eight sweeps: Gauss-Seidel on order 2's nodes is a weak
            // smoother - the multiplier's block 20 iterations of conjugate
            // gradients at two, 13 at four, 9 at eight; GMRES 140, 80, 52.)
            if (!c->compute(a, {{V, multigrid()}}, {0}, 8, e)) return nullptr;
            return c;
        };
    };
    // A and ψ's cycle: Gauss-Seidel on both; the gradients G of the gauge's
    // nodes in A (AMS's), the vertices' vector fields Π in A (AMS's), ψ
    // alone, and A = -∇φ with ψ = φ (Gc: the conductors' nodes' gradients) -
    // what K0 + ω K1 does not see, γ's alone.
    const auto rowsOf = [nS](const Eigen::SparseMatrix<double>& top, const Eigen::SparseMatrix<double>* bottom, int cols) {
        std::vector<T> t;
        for (int c = 0; c < top.outerSize(); ++c)
            for (Eigen::SparseMatrix<double>::InnerIterator it(top, c); it; ++it) t.emplace_back(int(it.row()), c, it.value());
        if (bottom)
            for (int c = 0; c < bottom->outerSize(); ++c)
                for (Eigen::SparseMatrix<double>::InnerIterator it(*bottom, c); it; ++it) t.emplace_back(int(top.rows() + it.row()), c, it.value());
        Eigen::SparseMatrix<double> m(nS, cols);
        m.setFromTriplets(t.begin(), t.end());
        return m;
    };
    const Eigen::SparseMatrix<double> G = columnsByEquation(sp.Gn, q.eqP, nP);
    std::vector<AuxiliaryCycle::Space> spaces;
    spaces.push_back({rowsOf(G, nullptr, nP), nodal(q.eqP, nP)});
    spaces.push_back({rowsOf(sp.Pi, nullptr, int(sp.Pi.cols())), multigrid(3)});
    std::vector<int> order{0, 1, 0};
    if (nV > 0) {
        Eigen::SparseMatrix<double> I(nV, nV);
        I.setIdentity();
        const Eigen::SparseMatrix<double> Gc = -columnsByEquation(sp.Gn, hs.h.eqV, nV);
        const Eigen::SparseMatrix<double> zero(nA, nV);
        spaces.push_back({rowsOf(zero, &I, nV), nodal(hs.h.eqV, nV)});
        spaces.push_back({rowsOf(Gc, &I, nV), nodal(hs.h.eqV, nV)});
        order = {0, 1, 2, 3, 2, 1, 0};
    }
    AuxiliaryCycle cycle;
    QString why;
    std::unique_ptr<Cycle> pp;
    // (Four sweeps on the edges: GMRES 140 iterations at two, 111 at four
    // on a coax at 10 Hz, 411 and 347 at 1 MHz.)
    bool made = cycle.compute(PS, std::move(spaces), order, 4, &why);
    if (made) made = (pp = nodal(q.eqP, nP)(PP, &why)) != nullptr;
    if (!made) {
        *error = tr("%1: the iterative solve could not be set up (%2)").arg(name, why);
        return false;
    }
    // The rows and columns scaled by the preconditioner's diagonal: each
    // block's residual weighed alike.
    Eigen::VectorXd D(hs.n);
    const Eigen::VectorXd dS = PS.diagonal();
    D.segment(0, nA) = dS.head(nA).cwiseSqrt().cwiseInverse();
    D.segment(nA, nP) = PP.diagonal().cwiseSqrt().cwiseInverse();
    if (nV > 0) D.segment(hs.oV, nV) = dS.tail(nV).cwiseAbs().cwiseSqrt().cwiseInverse();
    for (Eigen::Index k = 0; k < D.size(); ++k)
        if (!std::isfinite(D(k))) D(k) = 1;
    const double setUp = clock.elapsed() / 1000.0;
    // (K0 x + jω K1 x: K0 + jω K1 not made, its copy as large as the
    // preconditioner.)
    const auto op = [&](const Eigen::VectorXcd& x, Eigen::VectorXcd& y) {
        const Eigen::VectorXcd u = D.cwiseProduct(x);
        y = D.cwiseProduct(hs.K0 * u + jw * (hs.K1 * u));
    };
    const auto pre = [&](const Eigen::VectorXcd& x, Eigen::VectorXcd& y) {
        // (Real: the real and imaginary parts each, the scaling undone.)
        const Eigen::VectorXcd u = D.cwiseInverse().cwiseProduct(x);
        y.resize(x.size());
        Eigen::VectorXd vs(nS), out;
        for (int c = 0; c < 2; ++c) {
            const Eigen::VectorXd v = c == 0 ? Eigen::VectorXd(u.real()) : Eigen::VectorXd(u.imag());
            Eigen::VectorXd z(v.size());
            vs.head(nA) = v.head(nA);
            if (nV > 0) vs.tail(nV) = v.segment(hs.oV, nV);
            cycle.apply(vs, out);
            z.head(nA) = out.head(nA);
            if (nV > 0) z.segment(hs.oV, nV) = out.tail(nV);
            pp->apply(v.segment(nA, nP), out);
            z.segment(nA, nP) = out;
            if (c == 0) y.real() = z;
            else y.imag() = z;
        }
        y = D.cwiseInverse().cwiseProduct(y);
    };
    const double tolerance = std::min(o.tolerance > 0 ? o.tolerance : 1e-9, 1e-9);
    const int cols = 1 + int(hs.P.cols());
    X.resize(hs.n, cols * (1 + std::clamp(moments, 0, 2)));
    int iterations = 0, worstIterations = 0;
    double worst = 0;
    bool ok = true;
    const auto solve = [&](const Eigen::VectorXcd& b) {
        Eigen::VectorXcd xs = Eigen::VectorXcd::Zero(hs.n);
        const IterativeReport r = gmres(op, pre, D.cwiseProduct(b), xs, tolerance, 2000);
        iterations += r.iterations;
        worstIterations = std::max(worstIterations, r.iterations);
        worst = std::max(worst, r.residual);
        ok = ok && r.converged;
        return Eigen::VectorXcd(D.cwiseProduct(xs));
    };
    const cd j1(0, 1);
    for (int j = 0; j < cols; ++j) {
        const Eigen::VectorXcd b = j == 0 ? Eigen::VectorXcd(hs.bc + jw * hs.bj) : Eigen::VectorXcd(hs.P.col(j - 1));
        X.col(j) = solve(b);
        if (moments >= 1) {
            Eigen::VectorXcd d = -j1 * (hs.K1 * X.col(j));
            if (j == 0) d += j1 * hs.bj;
            X.col(cols + j) = solve(d);
        }
        if (moments >= 2) X.col(2 * cols + j) = solve(Eigen::VectorXcd(-2.0 * j1 * (hs.K1 * X.col(cols + j))));
    }
    a_s.log << tr("%1%2: %3 unknowns (%4 of A on edge elements of order %5, %6 of the gauge's multiplier, %7 of the conductors' potential), "
                  "GMRES with the auxiliary space preconditioner: set up in %8 s (%9 MB), %10 iterations (at most %11 a solve) in %12 s, "
                  "relative residual %13")
                   .arg(name, at)
                   .arg(hs.n)
                   .arg(nA)
                   .arg(es.order())
                   .arg(nP)
                   .arg(nV)
                   .arg(setUp, 0, 'f', 2)
                   .arg((cycle.bytes() + pp->bytes()) / 1048576.0, 0, 'f', 1)
                   .arg(iterations)
                   .arg(worstIterations)
                   .arg(clock.elapsed() / 1000.0 - setUp, 0, 'f', 2)
                   .arg(worst, 0, 'g', 2);
    if (!ok || !X.allFinite()) {
        *error = tr("%1: the iterative solve did not converge%2 (a relative residual of %3) - the direct solver (the step's Solver) solves it")
                     .arg(name, at)
                     .arg(worst, 0, 'g', 3);
        return false;
    }
    return true;
}

bool MagneticProblem3::solveExactly(const Options3& o, const Harmonic& hs, double omega, Eigen::MatrixXcd& X, QString* error, int moments)
{
    if (o.solver == QLatin1String("iterative")) return solveIteratively(o, hs, omega, X, error, moments);
    const QString name = a_node->name();
    const QString at = tr(" at %1").arg(formatQuantity(omega / (2 * M_PI), Dim::of(0, 0, -1)));
    const cd jw(0, omega);
    const Eigen::SparseMatrix<cd> M = hs.K0 + jw * hs.K1;
    o.tell(tr("%1: factorizing, %2 unknowns").arg(name).arg(M.rows()));
    QElapsedTimer clock;
    clock.start();
    SparseComplexLdlt lu;
    lu.compute(M);
    a_s.log << tr("%1%2: assembled in %3 s, %4 nonzeros; factorized in %5 s (%6%7)")
                   .arg(name, at)
                   .arg(hs.assembled, 0, 'f', 2)
                   .arg(M.nonZeros())
                   .arg(clock.elapsed() / 1000.0, 0, 'f', 2)
                   .arg(lu.name(), lu.factorBytes() > 0 ? tr("; its factors %1 MB%2")
                                                              .arg(lu.factorBytes() / 1048576.0, 0, 'f', 1)
                                                              .arg(lu.reusedAnalysis() ? tr(", its analysis kept from before") : QString())
                                                        : QString());
    if (lu.info() != Eigen::Success) {
        *error = tr("%1: its system could not be factorized (%2) - is A held somewhere (Magnetic Insulation, a Magnetic Potential)?")
                     .arg(name, QString::fromStdString(lu.lastErrorMessage()));
        return false;
    }
    o.tell(tr("%1: solving with its factors").arg(name));
    // [b(ω), the ports'], each refined once against the matrix; then their
    // derivatives in ω, asked.
    const int cols = 1 + int(hs.P.cols());
    X.resize(hs.n, cols * (1 + std::clamp(moments, 0, 2)));
    double residual = 0;
    const auto solveRefined = [&](const Eigen::VectorXcd& b) {
        Eigen::VectorXcd x = lu.solve(b);
        x += lu.solve(b - M * x);
        return x;
    };
    const cd j1(0, 1);
    for (int j = 0; j < cols; ++j) {
        const Eigen::VectorXcd b = j == 0 ? Eigen::VectorXcd(hs.bc + jw * hs.bj) : Eigen::VectorXcd(hs.P.col(j - 1));
        X.col(j) = solveRefined(b);
        const double bn = b.norm();
        if (j == 0) residual = bn > 0 ? (b - M * X.col(j)).norm() / bn : 0;
        if (moments >= 1) {
            Eigen::VectorXcd d = -j1 * (hs.K1 * X.col(j));
            if (j == 0) d += j1 * hs.bj;
            X.col(cols + j) = solveRefined(d);
        }
        if (moments >= 2) X.col(2 * cols + j) = solveRefined(Eigen::VectorXcd(-2.0 * j1 * (hs.K1 * X.col(cols + j))));
    }
    if (!X.allFinite()) {
        *error = tr("%1: its solve is not finite").arg(name);
        return false;
    }
    a_s.log << tr("%1%2: %3 unknowns (%4 of A on edge elements of order %5, %6 of the gauge's multiplier, %7 of the conductors' potential), "
                  "relative residual %8, refined once")
                   .arg(name, at)
                   .arg(M.rows())
                   .arg(hs.q.nA)
                   .arg(field().edges->order())
                   .arg(hs.q.nP)
                   .arg(hs.h.nV)
                   .arg(residual, 0, 'g', 2);
    return true;
}

void MagneticProblem3::finishHarmonic(const Options3& o, const Harmonic& hs, double omega, const Eigen::MatrixXcd& X)
{
    Field3& f = field();
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const QString name = a_node->name();
    const Conductors& h = hs.h;
    const Equations& q = hs.q;
    const int oV = hs.oV;
    const cd jw(0, omega);
    a_omega = omega;
    skinDepthCheck(h, omega);
    const std::size_t nE = std::size_t(es.size()), nN = std::size_t(ns.size());
    // A (by edge dof) and V (by node: jω ψ, or held) of a solution, the
    // held values its own (the main one's) or none (a port's).
    const auto unpack = [&](const Eigen::VectorXcd& sol, bool heldToo, std::vector<cd>& A, std::vector<cd>& V) {
        A.assign(nE, cd(0));
        for (std::size_t k = 0; k < nE; ++k) A[k] = q.eqA[k] >= 0 ? sol(q.eqA[k]) : q.eqA[k] == -3 && heldToo ? cd(hs.held[k]) : cd(0);
        V.assign(nN, cd(0));
        for (std::size_t k = 0; k < nN; ++k) V[k] = h.eqV[k] >= 0 ? jw * sol(oV + h.eqV[k]) : h.eqV[k] == -3 && heldToo ? h.heldV[k] : cd(0);
    };
    const Eigen::VectorXcd x = X.col(0);
    std::vector<cd> A, V;
    unpack(x, true, A, V);
    f.values.assign(2 * nE + 3 * nN, 0.0);
    for (std::size_t k = 0; k < nE; ++k) {
        f.values[k] = A[k].real();
        f.values[nE + nN + k] = A[k].imag();
    }
    for (std::size_t k = 0; k < nN; ++k) {
        f.values[nE + k] = q.eqP[k] >= 0 ? x(q.nA + q.eqP[k]).real() / hs.gaugeScale : 0.0;
        f.values[2 * nE + nN + k] = V[k].real();
        f.values[2 * nE + 2 * nN + k] = V[k].imag();
    }
    f.solved = true;
    // Each single conductor's current: what flows in at its terminal,
    // ∫ σ (jωA + ∇V)·∇h, h one on its terminal's nodes; turns' flux linkage
    // ∫ A·j, j their current density at 1 A.
    const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    const int nc = int(a_coils.size());
    const auto currents = [&](const std::vector<cd>& Av, const std::vector<cd>& Vv) {
        std::vector<cd> out(static_cast<std::size_t>(nc), cd(0));
        PointEvaluator3 ev(a_s);
        for (int e = 0; e < es.elements(); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !h.conducting[std::size_t(d)]) continue;
            const int* nd = ns.dofs(e);
            int which = -1;
            for (int k = 0; k < ns.perElement() && which < 0; ++k) which = h.terminalOf[std::size_t(nd[k])];
            if (which < 0) continue;
            const int* ed = es.dofs(e);
            const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
            const AffineMap am = affineMap(es.planNodes(), t);
            const double hz = es.z(layer + 1) - es.z(layer);
            const std::array<Expression, 9>& sg = a_sigma[std::size_t(d)];
            for (const auto& qp : rule.points) {
                EdgeSpace3::Basis b;
                es.basis(e, qp[0], qp[1], qp[2], b);
                ev.at(e, qp[0], qp[1], qp[2], b.x);
                PrismShape s;
                prismShapeAt(ns.order(), qp[0], qp[1], qp[2], s);
                double g[18][3];
                nodalGradients(s, am, hz, g);
                cd E[3] = {0, 0, 0};   // jωA + ∇V: -E
                for (int i = 0; i < b.count; ++i)
                    if (ed[i] >= 0)
                        for (int k = 0; k < 3; ++k) E[k] += jw * b.v[i][k] * Av[std::size_t(ed[i])];
                for (int k = 0; k < s.count; ++k)
                    for (int c = 0; c < 3; ++c) E[c] += g[k][c] * Vv[std::size_t(nd[k])];
                double s9[9];
                for (int k = 0; k < 9; ++k) {
                    s9[k] = sg[std::size_t(k)].isValid() ? ev.eval(sg[std::size_t(k)]) : 0.0;
                    if (!std::isfinite(s9[k])) s9[k] = 0;
                }
                for (int c = 0; c < nc; ++c) {
                    double gh[3] = {0, 0, 0};
                    bool any = false;
                    for (int k = 0; k < s.count; ++k)
                        if (h.terminalOf[std::size_t(nd[k])] == c) {
                            any = true;
                            for (int m = 0; m < 3; ++m) gh[m] += g[k][m];
                        }
                    if (!any) continue;
                    cd v = 0;
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) v += gh[i] * s9[3 * i + j] * E[j];
                    out[std::size_t(c)] += qp[3] * b.det * v;
                }
            }
        }
        return out;
    };
    const auto fluxOf = [&](int c, const std::vector<cd>& Av) {
        cd total = 0;
        PointEvaluator3 ev(a_s);
        for (int e = 0; e < es.elements(); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            const auto& ju = hs.unit[std::size_t(c)][std::size_t(d)];
            if (!ju[0].isValid() && !ju[1].isValid() && !ju[2].isValid()) continue;
            const int* ed = es.dofs(e);
            for (const auto& qp : rule.points) {
                EdgeSpace3::Basis b;
                es.basis(e, qp[0], qp[1], qp[2], b);
                ev.at(e, qp[0], qp[1], qp[2], b.x);
                cd a[3] = {0, 0, 0};
                for (int i = 0; i < b.count; ++i)
                    if (ed[i] >= 0)
                        for (int k = 0; k < 3; ++k) a[k] += b.v[i][k] * Av[std::size_t(ed[i])];
                for (int k = 0; k < 3; ++k)
                    if (ju[std::size_t(k)].isValid()) total += qp[3] * b.det * a[k] * ev.eval(ju[std::size_t(k)]);
            }
        }
        return total;
    };
    const auto voltageOf = [&](int c, const Eigen::VectorXcd& sol, const std::vector<cd>& Av, bool heldToo) -> cd {
        const Coil& coil = a_coils[std::size_t(c)];
        if (!coil.single) return jw * fluxOf(c, Av);
        if (h.eqU[std::size_t(c)] >= 0) return jw * sol(oV + h.eqU[std::size_t(c)]);
        if (heldToo)
            for (std::size_t k = 0; k < nN; ++k)
                if (h.terminalOf[k] == c) return h.heldV[k];
        return 0;
    };
    a_U.assign(std::size_t(nc), cd(0));
    a_I.assign(std::size_t(nc), cd(0));
    a_flux.assign(std::size_t(nc), cd(0));
    const std::vector<cd> found = currents(A, V);
    f.terminals.clear();
    a_ports = hs.ports;
    for (int c = 0; c < nc; ++c) {
        const Coil& coil = a_coils[std::size_t(c)];
        a_U[std::size_t(c)] = voltageOf(c, x, A, true);
        if (!coil.single) a_U[std::size_t(c)] += wireResistance(c) * evalAt(coil.current, Vec3{});
        if (coil.single) a_I[std::size_t(c)] = coil.role == Coil::Current ? cd(evalAt(coil.current, Vec3{})) : coil.role == Coil::Floating ? cd(0) : found[std::size_t(c)];
        else {
            a_I[std::size_t(c)] = evalAt(coil.current, Vec3{});
            a_flux[std::size_t(c)] = fluxOf(c, A);
        }
        Field3::Terminal t;
        t.name = coil.name;
        t.floating = std::find(hs.ports.begin(), hs.ports.end(), c) != hs.ports.end();
        t.potential = a_U[std::size_t(c)].real();
        t.charge = a_I[std::size_t(c)].real();
        f.terminals.push_back(t);
    }
    // A current held as given: what flows in as solved, said where it is
    // not (to a part in 10⁴).
    for (int c = 0; c < nc; ++c) {
        const Coil& coil = a_coils[std::size_t(c)];
        if (!coil.single || coil.role != Coil::Current) continue;
        const cd want = a_I[std::size_t(c)], got = found[std::size_t(c)];
        if (std::abs(got - want) > 1e-4 * std::max(std::abs(want), 1e-30))
            a_s.log << tr("%1: coil %2's current as solved, %3 A, is not its %4 A").arg(name, coil.name).arg(std::abs(got), 0, 'g', 6).arg(std::abs(want), 0, 'g', 6);
    }
    // Swept: each port fed 1 A in turn, the others none (a conductor's
    // voltage held at nought), no other source - their impedance matrix.
    a_Z.clear();
    if (o.sweep && !hs.ports.empty() && X.cols() == 1 + Eigen::Index(hs.ports.size())) {
        const int np = int(hs.ports.size());
        a_Z.assign(std::size_t(np), std::vector<cd>(std::size_t(np), cd(0)));
        for (int jp = 0; jp < np; ++jp) {
            const Eigen::VectorXcd xj = X.col(1 + jp);
            std::vector<cd> Aj, Vj;
            unpack(xj, false, Aj, Vj);
            for (int kp = 0; kp < np; ++kp) a_Z[std::size_t(kp)][std::size_t(jp)] = voltageOf(hs.ports[std::size_t(kp)], xj, Aj, false);
            // (A coil of turns' wire: its resistance on its own diagonal.)
            if (!a_coils[std::size_t(hs.ports[std::size_t(jp)])].single) a_Z[std::size_t(jp)][std::size_t(jp)] += wireResistance(hs.ports[std::size_t(jp)]);
        }
    }
}

// ------------------------------------------------------------- In time

void MagneticProblem3::beginStepping()
{
    // Its values the stepping's: A and the gauge's multiplier as they were
    // (at rest, or nought), ψ - the conductors' potential's time integral -
    // and each coil's nought; Ȧ and V nought.
    Field3& f = field();
    a_stepping.reset();
    // (After a Frequency Domain step: its ω no more - the start's results
    // at rest, not that step's.)
    a_omega = 0;
    if (!f.edges || !f.space) return;
    const std::size_t nE = std::size_t(f.edges->size()), nN = std::size_t(f.space->size()), nc = a_coils.size();
    std::vector<double> v(nE + 2 * nN + nc, 0.0);
    if (f.values.size() == nE + nN || f.values.size() == v.size())
        for (std::size_t k = 0; k < nE + nN; ++k) v[k] = f.values[k];
    if (f.values.size() == v.size()) v = f.values;
    f.values = std::move(v);
    f.internal.assign(nE + nN, 0.0);
    f.internalStride = 0;
}

bool MagneticProblem3::solveTransient(const Options3& o, QString* error)
{
    Field3& f = field();
    const EdgeSpace3& es = *f.edges;
    const Space3& ns = *f.space;
    const QString name = a_node->name();
    const std::size_t nE = std::size_t(es.size()), nN = std::size_t(ns.size());
    const int nc = int(a_coils.size());
    // (Its matrices the frequency domain's: K0 X + K1 Ẋ = b(t), X = (A,
    // its multiplier, ψ, the coils' ψ) - jω there d/dt here, ψ the
    // conductors' potential's time integral, V = ψ̇ -, assembled once a
    // stepping.)
    if (a_nonlinear) {
        *error = tr("%1: a B-H curve in time is not taken - give its domains a relative permeability for a study in time").arg(name);
        return false;
    }
    if (!a_stepping) {
        bool magneticLoss = false;
        const Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
        if (const auto it = coef.named.find(QStringLiteral("murIm")); it != coef.named.end())
            for (const Expression& e : it->second) magneticLoss = magneticLoss || (e.isValid() && !(e.isConstant() && e.constant() == 0));
        if (magneticLoss) a_s.log << tr("%1: µr'' (a core's losses) is the frequency domain's - left out in time").arg(name);
        auto hs = std::make_unique<Harmonic>();
        Options3 po = o;
        po.sweep = false;
        if (!prepareHarmonic(po, *hs, error)) return false;
        hs->K0 = hs->K0.real().cast<cd>();
        a_stepping = std::move(hs);
        a_steppingMass = 0;
        // (Its sources' and held voltages' part of b assembled once where none
        // reads t - each step's assembly over every element the most of a
        // step's work otherwise; a coil's current fed is added at each.)
        a_steppingR = Eigen::VectorXd();
        // (A coil of turns' term reads its currents physics' J, a field -
        // varying, to Solution3::varies() - but its part is a_steppingTurns',
        // that J fixed through the stepping: left out.)
        a_steppingVaries = false;
        for (const auto& j : sourcesWithoutTurns())
            for (const Expression& e : j) a_steppingVaries = a_steppingVaries || (e.isValid() && a_s.varies(e));
        for (const auto& face : a_sheets)
            for (const auto& j : face)
                for (const Expression& e : j) a_steppingVaries = a_steppingVaries || (e.isValid() && a_s.varies(e));
        // (The voltages held: their part of b linear in each - its terminal's
        // nodes at 1 V, its grounds' at nought -, assembled once by coil and
        // taken times its voltage at each step.)
        a_steppingVolt.assign(a_coils.size(), Eigen::VectorXd());
        const Conductors& hc = a_stepping->h;
        const std::vector<double> zero(nE, 0.0);
        const std::vector<std::array<Expression, 3>> J0 = sourcesOf(-1);
        for (int c = 0; c < nc; ++c) {
            if (a_coils[std::size_t(c)].role != Coil::Voltage) continue;
            std::vector<cd> unitV(nN, cd(0));
            bool any = false;
            for (std::size_t k = 0; k < nN; ++k)
                if (hc.eqV[k] == -3 && hc.terminalOf[k] == c) {
                    unitV[k] = 1.0;
                    any = true;
                }
            if (!any) continue;
            Eigen::SparseMatrix<cd> unusedM;
            Eigen::VectorXcd rv;
            assembleHarmonic(a_stepping->q, hc, 2, zero, unitV, J0, false, false, unusedM, rv, a_stepping->gaugeScale);
            a_steppingVolt[std::size_t(c)] = rv.real();
        }
        // (A coil of turns: its current density at 1 A's part of b, taken
        // times its current at each step - its current of t, not the one
        // its sources were built with.)
        a_steppingTurns.assign(a_coils.size(), Eigen::VectorXd());
        // (The wire's resistance taken once a stepping where its σw is
        // constant; one of t or of a field - a temperature - at each step.)
        a_steppingWire.assign(a_coils.size(), 0.0);
        a_steppingWireVaries.assign(a_coils.size(), 0);
        for (int c = 0; c < nc; ++c) {
            const Expression& sw = a_coils[std::size_t(c)].wireSigma;
            a_steppingWireVaries[std::size_t(c)] = sw.isValid() && a_s.varies(sw);
            a_steppingWire[std::size_t(c)] = wireResistance(c);
        }
        // (Nothing conducting K1 is nought: K0's factors kept whatever the
        // step - before, factorized again at each change of c0, 75 times in
        // an adaptive run of 55 steps.)
        a_steppingMassless = true;
        for (int k = 0; k < a_stepping->K1.nonZeros() && a_steppingMassless; ++k) a_steppingMassless = a_stepping->K1.valuePtr()[k] == cd(0);
        const std::vector<cd> noneV(nN, cd(0));
        for (int c = 0; c < nc; ++c) {
            if (a_coils[std::size_t(c)].single) continue;
            Eigen::SparseMatrix<cd> unusedM;
            Eigen::VectorXcd rj;
            assembleHarmonic(a_stepping->q, hc, 1, zero, noneV, a_stepping->unit[std::size_t(c)], false, false, unusedM, rj, a_stepping->gaugeScale);
            a_steppingTurns[std::size_t(c)] = rj.real();
        }
    }
    Harmonic& hs = *a_stepping;
    Conductors& h = hs.h;
    const Equations& q = hs.q;
    const int oV = hs.oV;
    const double c0 = o.mass;
    const std::vector<double> none;
    const std::vector<double>& hist = o.history ? *o.history : none;
    const auto histAt = [&hist](std::size_t k) { return k < hist.size() ? hist[k] : 0.0; };
    // Its right-hand side at this time: the sources, the voltages held, the
    // currents held - and K1 times the history.
    for (std::size_t k = 0; k < nN; ++k) {
        if (h.eqV[k] != -3) continue;
        const int c = h.terminalOf[k];
        if (c >= 0 && a_coils[std::size_t(c)].role == Coil::Voltage) h.heldV[k] = evalAt(a_coils[std::size_t(c)].voltage, Vec3{});
    }
    if (a_steppingVaries || a_steppingR.size() != hs.n) {
        o.tell(tr("%1: assembling at t = %2").arg(name, formatQuantity(a_s.time(), Dim::of(0, 0, 1))));
        // (The coils of turns left out: theirs added below.)
        const std::vector<std::array<Expression, 3>> J = sourcesWithoutTurns();
        const std::vector<cd> noneV(nN, cd(0));
        Eigen::SparseMatrix<cd> unused;
        Eigen::VectorXcd r0;
        assembleHarmonic(q, h, 1, hs.held, noneV, J, false, true, unused, r0, hs.gaugeScale);
        a_steppingR = r0.real();
    }
    Eigen::VectorXd b = a_steppingR;
    for (int c = 0; c < nc && c < int(a_steppingVolt.size()); ++c)
        if (a_steppingVolt[std::size_t(c)].size() == hs.n) b += evalAt(a_coils[std::size_t(c)].voltage, Vec3{}) * a_steppingVolt[std::size_t(c)];
    for (int c = 0; c < nc && c < int(a_steppingTurns.size()); ++c)
        if (a_steppingTurns[std::size_t(c)].size() == hs.n) b += evalAt(a_coils[std::size_t(c)].current, Vec3{}) * a_steppingTurns[std::size_t(c)];
    for (int c = 0; c < nc; ++c)
        if (h.eqU[std::size_t(c)] >= 0 && a_coils[std::size_t(c)].role == Coil::Current) b(oV + h.eqU[std::size_t(c)]) += evalAt(a_coils[std::size_t(c)].current, Vec3{});
    Eigen::VectorXd hx = Eigen::VectorXd::Zero(hs.n);
    for (std::size_t k = 0; k < nE; ++k)
        if (q.eqA[k] >= 0) hx(q.eqA[k]) = histAt(k);
    for (std::size_t k = 0; k < nN; ++k)
        if (h.eqV[k] >= 0) hx(oV + h.eqV[k]) = histAt(nE + nN + k);
    for (int c = 0; c < nc; ++c)
        if (h.eqU[std::size_t(c)] >= 0) hx(oV + h.eqU[std::size_t(c)]) = histAt(nE + 2 * nN + std::size_t(c));
    const Eigen::SparseMatrix<double> K1 = hs.K1.real();
    b += K1 * hx;
    // K0 + c0 K1, factorized again where c0 changed.
    if (!a_steppingLu || (!a_steppingMassless && std::abs(a_steppingMass - c0) > 1e-12 * c0)) {
        o.tell(tr("%1: factorizing, %2 unknowns").arg(name).arg(hs.n));
        const Eigen::SparseMatrix<double> M = Eigen::SparseMatrix<double>(hs.K0.real()) + c0 * K1;
        a_steppingLu = std::make_unique<SparseRealLdlt>();
        a_steppingLu->compute(M);
        if (a_steppingLu->info() != Eigen::Success) {
            *error = tr("%1: its system in time could not be factorized (%2)").arg(name, QString::fromStdString(a_steppingLu->lastErrorMessage()));
            a_steppingLu.reset();
            return false;
        }
        a_steppingMass = c0;
        a_s.log << tr("%1: in time, %2 unknowns (%3 of the conductors' potential), factorized for a step of %4 (%5)")
                       .arg(name)
                       .arg(hs.n)
                       .arg(h.nV)
                       .arg(formatQuantity(1 / c0, Dim::of(0, 0, 1)), a_steppingLu->name());
    }
    const Eigen::VectorXd x = a_steppingLu->solve(b);
    if (!x.allFinite()) {
        *error = tr("%1: its solve in time is not finite").arg(name);
        return false;
    }
    // Its values: A, the multiplier, ψ (a voltage held: integrated, (V +
    // the history)/c0), the coils' ψ; then Ȧ and V = ψ̇.
    f.values.assign(nE + 2 * nN + std::size_t(nc), 0.0);
    f.internal.assign(nE + nN, 0.0);
    f.internalStride = 0;
    for (std::size_t k = 0; k < nE; ++k) {
        const double A = q.eqA[k] >= 0 ? x(q.eqA[k]) : q.eqA[k] == -3 ? hs.held[k] : 0.0;
        f.values[k] = A;
        f.internal[k] = c0 * A - histAt(k);
    }
    for (std::size_t k = 0; k < nN; ++k) {
        f.values[nE + k] = q.eqP[k] >= 0 ? x(q.nA + q.eqP[k]) / hs.gaugeScale : 0.0;
        double psi = 0, V = 0;
        if (h.eqV[k] >= 0) {
            psi = x(oV + h.eqV[k]);
            V = c0 * psi - histAt(nE + nN + k);
        } else if (h.eqV[k] == -3) {
            V = h.heldV[k].real();
            psi = (V + histAt(nE + nN + k)) / c0;
        }
        f.values[nE + nN + k] = psi;
        f.internal[nE + k] = V;
    }
    for (int c = 0; c < nc; ++c)
        if (h.eqU[std::size_t(c)] >= 0) f.values[nE + 2 * nN + std::size_t(c)] = x(oV + h.eqU[std::size_t(c)]);
    // Each coil's current and voltage now: a single conductor's current fed,
    // or what flows in at its terminal, ∫ σ (Ȧ + ∇V)·∇h; its voltage held,
    // or ψ̇ of its terminal; turns' voltage ∫ Ȧ·j, j their current density
    // at 1 A.
    a_omega = 0;
    a_U.assign(std::size_t(nc), cd(0));
    a_I.assign(std::size_t(nc), cd(0));
    a_flux.assign(std::size_t(nc), cd(0));
    const Mesh3& mesh = a_s.mesh();
    const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    std::vector<double> found(std::size_t(nc), 0.0), turns(std::size_t(nc), 0.0);
    {
        PointEvaluator3 ev(a_s);
        for (int e = 0; e < es.elements(); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            const int* nd = ns.dofs(e);
            const int* ed = es.dofs(e);
            const bool conducting = h.conducting[std::size_t(d)];
            int which = -1;
            if (conducting)
                for (int k = 0; k < ns.perElement() && which < 0; ++k) which = h.terminalOf[std::size_t(nd[k])];
            bool anyTurns = false;
            for (int c = 0; c < nc; ++c)
                if (!a_coils[std::size_t(c)].single) {
                    const auto& ju = hs.unit[std::size_t(c)][std::size_t(d)];
                    anyTurns = anyTurns || ju[0].isValid() || ju[1].isValid() || ju[2].isValid();
                }
            if (which < 0 && !anyTurns) continue;
            const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
            const AffineMap am = affineMap(es.planNodes(), t);
            const double hz = es.z(layer + 1) - es.z(layer);
            const std::array<Expression, 9>& sg = a_sigma[std::size_t(d)];
            for (const auto& qp : rule.points) {
                EdgeSpace3::Basis bb;
                es.basis(e, qp[0], qp[1], qp[2], bb);
                ev.at(e, qp[0], qp[1], qp[2], bb.x);
                PrismShape s;
                prismShapeAt(ns.order(), qp[0], qp[1], qp[2], s);
                double g[18][3];
                nodalGradients(s, am, hz, g);
                double Ad[3] = {0, 0, 0};
                for (int i = 0; i < bb.count; ++i)
                    if (ed[i] >= 0)
                        for (int k = 0; k < 3; ++k) Ad[k] += bb.v[i][k] * f.internal[std::size_t(ed[i])];
                const double w = qp[3] * bb.det;
                for (int c = 0; c < nc; ++c) {
                    if (a_coils[std::size_t(c)].single) continue;
                    const auto& ju = hs.unit[std::size_t(c)][std::size_t(d)];
                    for (int k = 0; k < 3; ++k)
                        if (ju[std::size_t(k)].isValid()) turns[std::size_t(c)] += w * Ad[k] * ev.eval(ju[std::size_t(k)]);
                }
                if (which < 0) continue;
                double E[3] = {Ad[0], Ad[1], Ad[2]};   // Ȧ + ∇V: -E
                for (int k = 0; k < s.count; ++k)
                    for (int m = 0; m < 3; ++m) E[m] += g[k][m] * f.internal[nE + std::size_t(nd[k])];
                double s9[9];
                for (int k = 0; k < 9; ++k) {
                    s9[k] = sg[std::size_t(k)].isValid() ? ev.eval(sg[std::size_t(k)]) : 0.0;
                    if (!std::isfinite(s9[k])) s9[k] = 0;
                }
                for (int c = 0; c < nc; ++c) {
                    double gh[3] = {0, 0, 0};
                    bool any = false;
                    for (int k = 0; k < s.count; ++k)
                        if (h.terminalOf[std::size_t(nd[k])] == c) {
                            any = true;
                            for (int m = 0; m < 3; ++m) gh[m] += g[k][m];
                        }
                    if (!any) continue;
                    double v = 0;
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) v += gh[i] * s9[3 * i + j] * E[j];
                    found[std::size_t(c)] += w * v;
                }
            }
        }
    }
    f.terminals.clear();
    for (int c = 0; c < nc; ++c) {
        const Coil& coil = a_coils[std::size_t(c)];
        double I = 0, U = 0;
        if (coil.single) {
            I = coil.role == Coil::Current ? evalAt(coil.current, Vec3{}) : coil.role == Coil::Floating ? 0.0 : found[std::size_t(c)];
            if (h.eqU[std::size_t(c)] >= 0) U = c0 * f.values[nE + 2 * nN + std::size_t(c)] - histAt(nE + 2 * nN + std::size_t(c));
            else
                for (std::size_t k = 0; k < nN; ++k)
                    if (h.terminalOf[k] == c) {
                        U = f.internal[nE + k];
                        break;
                    }
        } else {
            I = evalAt(coil.current, Vec3{});
            const bool cached = c < int(a_steppingWire.size()) && !a_steppingWireVaries[std::size_t(c)];
            U = turns[std::size_t(c)] + (cached ? a_steppingWire[std::size_t(c)] : wireResistance(c)) * I;
        }
        a_I[std::size_t(c)] = I;
        a_U[std::size_t(c)] = U;
        Field3::Terminal tm;
        tm.name = coil.name;
        tm.potential = U;
        tm.charge = I;
        f.terminals.push_back(tm);
    }
    a_inTime = true;
    f.solved = true;
    return true;
}

bool MagneticProblem3::solveHarmonic(const Options3& o, QString* error)
{
    const double omega = 2 * M_PI * o.frequency;
    if (!a_nonlinear) {
        a_muEff.clear();
        Harmonic hs;
        if (!prepareHarmonic(o, hs, error)) return false;
        Eigen::MatrixXcd X;
        if (!solveExactly(o, hs, omega, X, error)) return false;
        finishHarmonic(o, hs, omega, X);
        return true;
    }
    // A B-H curve: each element of it its effective µr of its B's amplitude
    // (effectiveMur()), from the curve's first slope, solved in turn - the
    // change relaxed - till none changes by more than the tolerance.
    Field3& f = field();
    const Mesh3& mesh = a_s.mesh();
    const Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const QString name = a_node->name();
    const PrismRule& rule = f.edges->order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    const std::size_t nq = rule.points.size(), np = mesh.prisms.size() * nq;
    a_muEff.assign(np, 0.0);
    for (std::size_t e = 0; e < mesh.prisms.size(); ++e) {
        const int d = mesh.domain[e];
        if (d >= 0 && d < int(coef.curves.size()) && coef.curves[std::size_t(d)])
            for (std::size_t k = 0; k < nq; ++k) a_muEff[e * nq + k] = 1 / (Mu0 * coef.curves[std::size_t(d)]->nu(0));
    }
    // (µr to a part in a thousand: the field's own error larger.)
    const double tol = std::max(o.tolerance, 1e-3);
    double change = 1;
    int it = 0;
    std::map<const BHCurve*, std::unique_ptr<EffectiveTable>> tables;
    for (const auto& c : coef.curves)
        if (c && !tables.count(c.get())) tables[c.get()] = std::make_unique<EffectiveTable>(*c);
    // Each element's bracket of µr, its effective curve's range at first -
    // a µr its B̂ is consistent with lies in it.
    std::vector<double> lo(np, 0.0), hi(np, 0.0);
    for (std::size_t e = 0; e < np; ++e) {
        if (!(a_muEff[e] > 0)) continue;
        const BHCurve& cv = *coef.curves[std::size_t(mesh.domain[e / nq])];
        const EffectiveTable& tb = *tables[&cv];
        double least = 1e300, most = 0;
        for (std::size_t k = 1; k < tb.b.size(); ++k) {
            const double mu = tb.b[k] / (Mu0 * tb.h[k]);
            least = std::min(least, mu);
            most = std::max(most, mu);
        }
        most = std::max(most, a_muEff[e]);
        lo[e] = least;
        hi[e] = most;
    }
    for (; it < std::max(2, o.maxIterations); ++it) {
        if (o.cancelled && o.cancelled()) {
            *error = tr("cancelled");
            return false;
        }
        Harmonic hs;
        if (!prepareHarmonic(o, hs, error)) return false;
        Eigen::MatrixXcd X;
        if (!solveExactly(o, hs, omega, X, error)) return false;
        finishHarmonic(o, hs, omega, X);
        f.internal = a_muEff;
        f.internalStride = int(nq);
        if (change < tol) break;
        // Each element's B's amplitude at its middle and the µr its curve
        // gives it: above its µr, the µr sought is more (its bracket's floor
        // raised), else less (its ceiling lowered); the next its bracket's
        // geometric mean - a bisection in ln µr, whatever drives it (µr of B̂
        // taken as the next swung by a factor of 80 round a ring fed a
        // current, saturation's slope past what a fixed point bears).
        change = 0;
        PointEvaluator3 ev(a_s);
        std::vector<double> next = a_muEff;
        for (std::size_t e = 0; e < np; ++e) {
            if (!(a_muEff[e] > 0)) continue;
            const auto& qp = rule.points[e % nq];
            ev.at(int(e / nq), qp[0], qp[1], qp[2]);
            MagneticState st;
            if (!magneticStateAt(ev, a_fi, st)) continue;
            double b2 = 0;
            for (int k = 0; k < 3; ++k) b2 += st.B[k] * st.B[k] + st.Bi[k] * st.Bi[k];
            const BHCurve& cv = *coef.curves[std::size_t(mesh.domain[e / nq])];
            const EffectiveTable& tb = *tables[&cv];
            const double mu0r = 1 / (Mu0 * cv.nu(0)), bh = std::sqrt(b2);
            const double wanted = tb.murOfB(bh, mu0r);
            (wanted > a_muEff[e] ? lo[e] : hi[e]) = a_muEff[e];
            const double mu = std::sqrt(lo[e] * hi[e]);
            change = std::max(change, std::abs(mu - a_muEff[e]) / a_muEff[e]);
            next[e] = mu;
        }
        a_muEff = std::move(next);
    }
    a_s.log << tr("%1: its B-H curve's effective permeability in %2 solves, its last change %3").arg(name).arg(it + 1).arg(change, 0, 'g', 2);
    if (change >= tol) {
        *error = tr("%1: its B-H curve's effective permeability did not settle in %2 solves").arg(name).arg(it + 1);
        return false;
    }
    return true;
}

bool MagneticProblem3::sweepFrequencies(const Options3& o, const std::vector<double>& frequencies, double tolerance,
                                        const std::function<bool(int)>& each, QString* error)
{
    const QString name = a_node->name();
    const int Nf = int(frequencies.size());
    // (A B-H curve: its matrix of its field's own amplitude - each frequency
    // solved, no fast sweep.)
    if (a_nonlinear) {
        error->clear();
        return false;
    }
    Harmonic hs;
    if (!prepareHarmonic(o, hs, error)) return false;
    QElapsedTimer clock;
    clock.start();
    const int cols = 1 + int(hs.P.cols());
    // The rows' scale, |K0 + jω K1|'s diagonal (the gauge's rows, none: the
    // edges' mean): a residual of the conductors' rows - jωσ's, small at a
    // low frequency - weighed as one of A's.
    const Eigen::VectorXcd d0 = hs.K0.diagonal(), d1 = hs.K1.diagonal();
    double meanA = 0;
    for (int k = 0; k < hs.q.nA; ++k) meanA += std::abs(d0(k)) / std::max(1, hs.q.nA);
    const auto weightsAt = [&](double omega) {
        Eigen::VectorXd w(hs.n);
        for (int k = 0; k < hs.n; ++k) {
            const double dk = std::abs(d0(k) + cd(0, omega) * d1(k));
            w(k) = 1 / std::sqrt(dk > 0 ? dk : std::max(meanA, 1e-300));
        }
        return w;
    };
    // The reduced basis: the solutions at the frequencies solved exactly,
    // orthonormal; K0 and K1 on it (Vᵀ, no conjugate: their symmetry kept).
    Eigen::MatrixXcd V(hs.n, 0);
    Eigen::MatrixXcd R0, R1, Vbc, Vbj, VP;
    const auto grow = [&](const Eigen::MatrixXcd& X) {
        for (int j = 0; j < int(X.cols()); ++j) {
            Eigen::VectorXcd v = X.col(j);
            const double before = v.norm();
            if (!(before > 0)) continue;
            for (int pass = 0; pass < 2; ++pass)
                if (V.cols() > 0) v -= V * (V.adjoint() * v);
            const double after = v.norm();
            if (!(after > 1e-10 * before)) continue;
            V.conservativeResize(Eigen::NoChange, V.cols() + 1);
            V.col(V.cols() - 1) = v / after;
        }
        R0 = V.transpose() * (hs.K0 * V);
        R1 = V.transpose() * (hs.K1 * V);
        Vbc = V.transpose() * hs.bc;
        Vbj = V.transpose() * hs.bj;
        VP = V.transpose() * hs.P;
    };
    // Solutions X at frequency k: their residual - the columns' worst,
    // weighed, of their right-hand side.
    const auto residualOf = [&](int k, const Eigen::MatrixXcd& X) {
        const double omega = 2 * M_PI * frequencies[std::size_t(k)];
        const cd jw(0, omega);
        const Eigen::VectorXd w = weightsAt(omega);
        double worst = 0;
        for (int j = 0; j < cols; ++j) {
            const Eigen::VectorXcd b = j == 0 ? Eigen::VectorXcd(hs.bc + jw * hs.bj) : Eigen::VectorXcd(hs.P.col(j - 1));
            const Eigen::VectorXcd r = b - hs.K0 * X.col(j) - jw * (hs.K1 * X.col(j));
            const double nb = w.cwiseProduct(b.cwiseAbs()).norm();
            if (nb > 0) worst = std::max(worst, w.cwiseProduct(r.cwiseAbs()).norm() / nb);
        }
        return worst;
    };
    // A frequency on the basis: its coefficients, and its residual.
    const auto reduced = [&](int k, Eigen::MatrixXcd& Y) {
        const cd jw(0, 2 * M_PI * frequencies[std::size_t(k)]);
        const Eigen::MatrixXcd Ar = R0 + jw * R1;
        Eigen::MatrixXcd Br(V.cols(), cols);
        Br.col(0) = Vbc + jw * Vbj;
        if (cols > 1) Br.rightCols(cols - 1) = VP;
        Y = Ar.partialPivLu().solve(Br);
        return residualOf(k, V * Y);
    };
    std::vector<int> byFrequency(static_cast<std::size_t>(Nf));
    for (int k = 0; k < Nf; ++k) byFrequency[std::size_t(k)] = k;
    std::sort(byFrequency.begin(), byFrequency.end(), [&](int a, int b) { return frequencies[std::size_t(a)] < frequencies[std::size_t(b)]; });
    // (A frequency solved exactly kept as solved - its results its own
    // solve's, not the basis' rebuilding of it.)
    std::vector<char> exact(static_cast<std::size_t>(Nf), 0);
    std::vector<Eigen::MatrixXcd> solvedX(static_cast<std::size_t>(Nf));
    const auto solveAt = [&](int k) {
        Eigen::MatrixXcd X;
        if (!solveExactly(o, hs, 2 * M_PI * frequencies[std::size_t(k)], X, error, 2)) return false;
        exact[std::size_t(k)] = 1;
        solvedX[std::size_t(k)] = X.leftCols(cols);
        grow(X);
        return true;
    };
    // Its band's ends and middle first; then where the residual is largest,
    // until every one's is under the tolerance.
    std::vector<int> start;
    if (Nf <= 3) start = byFrequency;
    else start = {byFrequency.front(), byFrequency[std::size_t(Nf / 2)], byFrequency.back()};
    for (int k : start) {
        if (o.cancelled && o.cancelled()) {
            *error = tr("cancelled");
            return false;
        }
        o.tell(tr("%1: the fast sweep, solved exactly at %2").arg(name, formatQuantity(frequencies[std::size_t(k)], Dim::of(0, 0, -1))));
        if (!solveAt(k)) return false;
    }
    std::vector<Eigen::MatrixXcd> Y(static_cast<std::size_t>(Nf));
    std::vector<double> residual(static_cast<std::size_t>(Nf), 0.0);
    int rounds = 0;
    for (;;) {
        ++rounds;
        double worst = 0;
        int at = -1;
        for (int k = 0; k < Nf; ++k) {
            if (o.cancelled && o.cancelled()) {
                *error = tr("cancelled");
                return false;
            }
            residual[std::size_t(k)] = reduced(k, Y[std::size_t(k)]);
            if (!exact[std::size_t(k)] && residual[std::size_t(k)] > worst) {
                worst = residual[std::size_t(k)];
                at = k;
            }
        }
        if (at < 0 || worst <= tolerance) break;
        o.tell(tr("%1: the fast sweep, solved exactly at %2 (its residual %3)")
                   .arg(name, formatQuantity(frequencies[std::size_t(at)], Dim::of(0, 0, -1)))
                   .arg(worst, 0, 'g', 2));
        if (!solveAt(at)) return false;
    }
    // Every frequency's residual: on the basis, or its own solve's.
    int solved = 0;
    double largest = 0, largestExact = 0;
    QStringList where;
    for (int k : byFrequency) {
        if (exact[std::size_t(k)]) {
            ++solved;
            where << formatQuantity(frequencies[std::size_t(k)], Dim::of(0, 0, -1));
            residual[std::size_t(k)] = residualOf(k, solvedX[std::size_t(k)]);
            largestExact = std::max(largestExact, residual[std::size_t(k)]);
        }
        largest = std::max(largest, residual[std::size_t(k)]);
    }
    a_s.log << tr("%1: fast sweep, %2 frequencies, %3 solved exactly (%4), the rest on their solutions' reduced basis of %5 - its largest "
                  "residual %6 of every frequency's, %10 of those solved exactly (tolerance %7), %8 rounds, %9 s")
                   .arg(name)
                   .arg(Nf)
                   .arg(solved)
                   .arg(where.join(QLatin1String(", ")))
                   .arg(V.cols())
                   .arg(largest, 0, 'g', 2)
                   .arg(tolerance, 0, 'g', 2)
                   .arg(rounds)
                   .arg(clock.elapsed() / 1000.0, 0, 'f', 2)
                   .arg(largestExact, 0, 'g', 2);
    // Each frequency's fields and results in turn, as its own solve's, then
    // told.
    for (int k = 0; k < Nf; ++k) {
        const Eigen::MatrixXcd X = exact[std::size_t(k)] ? solvedX[std::size_t(k)] : Eigen::MatrixXcd(V * Y[std::size_t(k)]);
        finishHarmonic(o, hs, 2 * M_PI * frequencies[std::size_t(k)], X);
        if (!each(k)) {
            if (error->isEmpty()) *error = tr("cancelled");
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- Globals

void MagneticProblem3::setHarmonicGlobals()
{
    Solution3& s = a_s;
    Field3& f = field();
    const EdgeSpace3& es = *f.edges;
    const Mesh3& mesh = s.mesh();
    const QString p = f.tag + QLatin1Char('.');
    const Dim henry = Dim::of(2, 1, -2, -2), weber = Dim::of(2, 1, -2, -1);
    a_energy = a_losses = a_magneticLosses = 0;
    const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    PointEvaluator3 ev(s);
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        for (const auto& qp : rule.points) {
            EdgeSpace3::Basis b;
            es.basis(e, qp[0], qp[1], qp[2], b);
            ev.at(e, qp[0], qp[1], qp[2], b.x);
            MagneticState st;
            if (!magneticStateAt(ev, a_fi, st, true)) continue;
            const double w = qp[3] * b.det;
            if (std::isfinite(st.energy)) a_energy += w * st.energy;
            if (std::isfinite(st.q)) a_losses += w * st.q;
            if (std::isfinite(st.qm)) a_magneticLosses += w * st.qm;
        }
    }
    s.setGlobal(p + QStringLiteral("W"), a_energy, Dim::of(2, 1, -2), tr("magnetic energy, its time average"));
    s.setGlobal(p + QStringLiteral("P"), a_losses, Dim::power(), tr("resistive losses, their time average"));
    s.setGlobal(p + QStringLiteral("Pm"), a_magneticLosses, Dim::power(), tr("magnetic losses (a core's, of its µr''), their time average"));
    setForceGlobals(true);
    // Each coil's Lorentz force, its time average ∫ ½ Re(J × B*) over it (J
    // its sources' and its eddy currents').
    {
        const int nc = int(a_coils.size());
        std::vector<std::array<double, 3>> Fc(std::size_t(nc), {0, 0, 0});
        for (int e = 0; e < es.elements(); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            int which = -1;
            for (int c = 0; c < nc && which < 0; ++c)
                if (d < int(a_coils[std::size_t(c)].on.size()) && a_coils[std::size_t(c)].on[std::size_t(d)]) which = c;
            if (which < 0) continue;
            for (const auto& qp : rule.points) {
                EdgeSpace3::Basis b;
                es.basis(e, qp[0], qp[1], qp[2], b);
                ev.at(e, qp[0], qp[1], qp[2], b.x);
                MagneticState st;
                if (!magneticStateAt(ev, a_fi, st, true)) continue;
                const double w = 0.5 * qp[3] * b.det;
                std::array<double, 3>& F = Fc[std::size_t(which)];
                F[0] += w * (st.J[1] * st.B[2] - st.J[2] * st.B[1] + st.Ji[1] * st.Bi[2] - st.Ji[2] * st.Bi[1]);
                F[1] += w * (st.J[2] * st.B[0] - st.J[0] * st.B[2] + st.Ji[2] * st.Bi[0] - st.Ji[0] * st.Bi[2]);
                F[2] += w * (st.J[0] * st.B[1] - st.J[1] * st.B[0] + st.Ji[0] * st.Bi[1] - st.Ji[1] * st.Bi[0]);
            }
        }
        static const char* const axes[3] = {"x", "y", "z"};
        for (int c = 0; c < nc; ++c)
            for (int k = 0; k < 3; ++k)
                s.setGlobal(p + QStringLiteral("F") + QString::fromLatin1(axes[k]) + QLatin1Char('_') + a_coils[std::size_t(c)].name, Fc[std::size_t(c)][std::size_t(k)],
                            Dim::of(1, 1, -2), tr("the Lorentz force on coil %1, %2 component, its time average").arg(a_coils[std::size_t(c)].name, QString::fromLatin1(axes[k])));
    }
    for (int c = 0; c < int(a_coils.size()) && c < int(a_U.size()); ++c) {
        const Coil& coil = a_coils[std::size_t(c)];
        const QString n = coil.name;
        const cd I = a_I[std::size_t(c)], U = a_U[std::size_t(c)];
        const bool found = coil.single && coil.role != Coil::Current;
        s.setGlobal(p + QStringLiteral("I_") + n, I.real(), Dim::current(),
                    found ? tr("coil %1's current, found (its role holds its voltage), its real part").arg(n) : tr("coil %1's current").arg(n));
        if (found) s.setGlobal(p + QStringLiteral("I_") + n + QStringLiteral("_im"), I.imag(), Dim::current(), tr("coil %1's current, its imaginary part").arg(n));
        s.setGlobal(p + QStringLiteral("V_") + n, U.real(), Dim::voltage(), tr("coil %1's voltage, its real part").arg(n));
        s.setGlobal(p + QStringLiteral("V_") + n + QStringLiteral("_im"), U.imag(), Dim::voltage(), tr("coil %1's voltage, its imaginary part").arg(n));
        if (!coil.single) {
            s.setGlobal(p + QStringLiteral("Phi_") + n, a_flux[std::size_t(c)].real(), weber, tr("coil %1's flux linkage, its real part").arg(n));
            s.setGlobal(p + QStringLiteral("Phi_") + n + QStringLiteral("_im"), a_flux[std::size_t(c)].imag(), weber,
                        tr("coil %1's flux linkage, its imaginary part").arg(n));
        }
        if (std::abs(I) > 0) {
            const cd z = U / I;
            s.setGlobal(p + QStringLiteral("R_") + n, z.real(), Dim::resistance(), tr("coil %1's resistance, V/I's real part").arg(n));
            s.setGlobal(p + QStringLiteral("L_") + n, z.imag() / a_omega, henry, tr("coil %1's inductance, V/I's imaginary part over ω").arg(n));
            s.setGlobal(p + QStringLiteral("Z_") + n, std::abs(z), Dim::resistance(), tr("coil %1's impedance, |V/I|").arg(n));
            if (z.real() != 0) s.setGlobal(p + QStringLiteral("Q_") + n, z.imag() / z.real(), Dim(), tr("coil %1's quality factor, ωL/R").arg(n));
        }
    }
    const int np = int(a_Z.size());
    for (int i = 0; i < np; ++i)
        for (int j = 0; j < np; ++j) {
            const QString a = a_coils[std::size_t(a_ports[std::size_t(i)])].name, b = a_coils[std::size_t(a_ports[std::size_t(j)])].name;
            const cd z = a_Z[std::size_t(i)][std::size_t(j)];
            s.setGlobal(p + QStringLiteral("L_") + a + QLatin1Char('_') + b, z.imag() / a_omega, henry, tr("inductance matrix, %1 %2").arg(a, b));
            s.setGlobal(p + QStringLiteral("R_") + a + QLatin1Char('_') + b, z.real(), Dim::resistance(), tr("resistance matrix, %1 %2").arg(a, b));
            if (a.size() == 1 && b.size() == 1) {
                s.setGlobal(p + QStringLiteral("L") + a + b, z.imag() / a_omega, henry, tr("inductance matrix, %1 %2").arg(a, b));
                s.setGlobal(p + QStringLiteral("R") + a + b, z.real(), Dim::resistance(), tr("resistance matrix, %1 %2").arg(a, b));
            }
        }
}

void MagneticProblem3::setGlobals()
{
    if (a_omega > 0) {
        setHarmonicGlobals();
        return;
    }
    Solution3& s = a_s;
    Field3& f = field();
    const EdgeSpace3& es = *f.edges;
    const Mesh3& mesh = s.mesh();
    const QString p = f.tag + QLatin1Char('.');
    const Solution3::Coefficients& coef = s.coefficients()[std::size_t(a_fi)];
    const Dim henry = Dim::of(2, 1, -2, -2), weber = Dim::of(2, 1, -2, -1), force = Dim::of(1, 1, -2);
    // The energy, ∫ Wm; each coil's flux linkage ∫ A·J / I0 and Lorentz force
    // ∫ J × B.
    a_energy = 0;
    for (Coil& c : a_coils) {
        c.flux = 0;
        c.force[0] = c.force[1] = c.force[2] = 0;
    }
    const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    PointEvaluator3 ev(s);
    const std::vector<std::array<Expression, 3>> J = sourcesOf(-1);
    std::vector<std::vector<std::array<Expression, 3>>> unit;
    for (int c = 0; c < int(a_coils.size()); ++c) unit.push_back(sourcesOf(c));
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        for (const auto& qp : rule.points) {
            EdgeSpace3::Basis b;
            es.basis(e, qp[0], qp[1], qp[2], b);
            ev.at(e, qp[0], qp[1], qp[2], b.x);
            const double w = qp[3] * b.det;
            MagneticState st;
            if (!magneticStateAt(ev, a_fi, st)) continue;
            if (std::isfinite(st.energy)) a_energy += w * st.energy;
            // (In time a single conductor's force ∫ J × B, J its eddy
            // currents' σE and its sources' - its sources found, not given:
            // sourcesOf() has none of it.)
            MagneticState sj;
            bool currents = false;
            for (int c = 0; c < int(a_coils.size()); ++c) {
                Coil& single = a_coils[std::size_t(c)];
                if (!a_inTime || !single.single || !single.on[std::size_t(d)]) continue;
                if (!currents && !magneticStateAt(ev, a_fi, sj, true)) break;
                currents = true;
                single.force[0] += w * (sj.J[1] * sj.B[2] - sj.J[2] * sj.B[1]);
                single.force[1] += w * (sj.J[2] * sj.B[0] - sj.J[0] * sj.B[2]);
                single.force[2] += w * (sj.J[0] * sj.B[1] - sj.J[1] * sj.B[0]);
            }
            for (int c = 0; c < int(a_coils.size()); ++c) {
                if (a_inTime && a_coils[std::size_t(c)].single) continue;
                const auto& ju = unit[std::size_t(c)][std::size_t(d)];
                if (!ju[0].isValid() && !ju[1].isValid() && !ju[2].isValid()) continue;
                double jv[3];
                for (int k = 0; k < 3; ++k) jv[k] = ju[std::size_t(k)].isValid() ? ev.eval(ju[std::size_t(k)]) : 0.0;
                Coil& coil = a_coils[std::size_t(c)];
                coil.flux += w * (st.A[0] * jv[0] + st.A[1] * jv[1] + st.A[2] * jv[2]);
                const double i0 = evalAt(coil.current, Vec3{});
                const double j[3] = {jv[0] * i0, jv[1] * i0, jv[2] * i0};
                coil.force[0] += w * (j[1] * st.B[2] - j[2] * st.B[1]);
                coil.force[1] += w * (j[2] * st.B[0] - j[0] * st.B[2]);
                coil.force[2] += w * (j[0] * st.B[1] - j[1] * st.B[0]);
            }
        }
    }
    (void)coef;
    (void)J;
    s.setGlobal(p + QStringLiteral("W"), a_energy, Dim::of(2, 1, -2), tr("magnetic energy"));
    for (const Coil& c : a_coils) {
        const double i0 = evalAt(c.current, Vec3{});
        s.setGlobal(p + QStringLiteral("I_") + c.name, i0, Dim::current(), tr("coil %1's current").arg(c.name));
        // (In time a single conductor's flux linkage is not one - its
        // current's density found, uneven, its voltage V_<coil> what it
        // has -: NaN, said, not left out, which would show an earlier time's
        // - a snapshot's globals are its own laid over the last ones.)
        if (a_inTime && c.single) {
            const double nan = std::numeric_limits<double>::quiet_NaN();
            s.setGlobal(p + QStringLiteral("Phi_") + c.name, nan, weber, tr("coil %1's flux linkage: none in time (a single conductor's current found, uneven)").arg(c.name));
            s.setGlobal(p + QStringLiteral("L_") + c.name, nan, henry, tr("coil %1's inductance: none in time (a single conductor's current found, uneven)").arg(c.name));
        } else {
            s.setGlobal(p + QStringLiteral("Phi_") + c.name, c.flux, weber, tr("coil %1's flux linkage").arg(c.name));
            if (i0 != 0) s.setGlobal(p + QStringLiteral("L_") + c.name, c.flux / i0, henry, tr("coil %1's inductance: its flux linkage over its current").arg(c.name));
        }
        static const char* const axes[3] = {"x", "y", "z"};
        for (int k = 0; k < 3; ++k)
            s.setGlobal(p + QStringLiteral("F") + QString::fromLatin1(axes[k]) + QLatin1Char('_') + c.name, c.force[k], force,
                        tr("the Lorentz force on coil %1, %2 component").arg(c.name, QString::fromLatin1(axes[k])));
        // (A coil of turns' wire: its DC resistance, its voltage R I0.)
        if (!c.single && c.wireSigma.isValid() && !a_inTime) {
            const double R = wireResistance(int(&c - a_coils.data()));
            s.setGlobal(p + QStringLiteral("R_") + c.name, R, Dim::resistance(), tr("coil %1's wire resistance").arg(c.name));
            s.setGlobal(p + QStringLiteral("V_") + c.name, R * i0, Dim::voltage(), tr("coil %1's voltage at rest, R I0").arg(c.name));
        }
    }
    const int nc = int(a_inductance.size());
    for (int i = 0; i < nc; ++i)
        for (int j = 0; j < nc; ++j) {
            const QString a = a_coils[std::size_t(i)].name, b = a_coils[std::size_t(j)].name;
            const double l = a_inductance[std::size_t(i)][std::size_t(j)];
            s.setGlobal(p + QStringLiteral("L_") + a + QLatin1Char('_') + b, l, henry, tr("inductance matrix, %1 %2").arg(a, b));
            if (a.size() == 1 && b.size() == 1) s.setGlobal(p + QStringLiteral("L") + a + b, l, henry, tr("inductance matrix, %1 %2").arg(a, b));
        }
    setForceGlobals(false);
    // In time: each coil's current and voltage now, and the resistive losses
    // ∫ σ |E|² now.
    if (a_inTime) {
        for (int c = 0; c < int(a_coils.size()) && c < int(a_I.size()); ++c) {
            const QString n = a_coils[std::size_t(c)].name;
            s.setGlobal(p + QStringLiteral("I_") + n, a_I[std::size_t(c)].real(), Dim::current(), tr("coil %1's current now").arg(n));
            s.setGlobal(p + QStringLiteral("V_") + n, a_U[std::size_t(c)].real(), Dim::voltage(), tr("coil %1's voltage now (its EMF and its conductor's drop)").arg(n));
        }
        double P = 0;
        for (int e = 0; e < es.elements(); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)] || !conducts(d)) continue;
            for (const auto& qp : rule.points) {
                EdgeSpace3::Basis b;
                es.basis(e, qp[0], qp[1], qp[2], b);
                ev.at(e, qp[0], qp[1], qp[2], b.x);
                MagneticState st;
                if (magneticStateAt(ev, a_fi, st, true) && std::isfinite(st.q)) P += qp[3] * b.det * st.q;
            }
        }
        s.setGlobal(p + QStringLiteral("P"), P, Dim::power(), tr("resistive losses now (its eddy currents')"));
    }
}

double MagneticProblem3::wireResistance(int c) const
{
    if (c < 0 || c >= int(a_coils.size())) return 0;
    const Coil& coil = a_coils[std::size_t(c)];
    if (coil.single || !coil.wireSigma.isValid()) return 0;
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const EdgeSpace3& es = *f.edges;
    const Mesh3& mesh = a_s.mesh();
    const std::vector<std::array<Expression, 3>> unit = sourcesOf(c);
    const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    PointEvaluator3 ev(a_s);
    double R = 0;
    for (int e = 0; e < es.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || d >= int(unit.size()) || !f.domains[std::size_t(d)]) continue;
        const auto& ju = unit[std::size_t(d)];
        if (!ju[0].isValid() && !ju[1].isValid() && !ju[2].isValid()) continue;
        for (const auto& qp : rule.points) {
            EdgeSpace3::Basis b;
            es.basis(e, qp[0], qp[1], qp[2], b);
            ev.at(e, qp[0], qp[1], qp[2], b.x);
            double j2 = 0;
            for (int k = 0; k < 3; ++k)
                if (ju[std::size_t(k)].isValid()) {
                    const double v = ev.eval(ju[std::size_t(k)]);
                    if (std::isfinite(v)) j2 += v * v;
                }
            const double sw = ev.eval(coil.wireSigma);
            if (sw > 0 && std::isfinite(sw)) R += qp[3] * b.det * j2 / (sw * coil.fill);
        }
    }
    return R;
}

void MagneticProblem3::setForceGlobals(bool harmonic)
{
    if (a_forces.empty()) return;
    Solution3& s = a_s;
    Field3& f = field();
    if (!f.space) return;
    const Space3& sp = *f.space;
    const Mesh3& mesh = s.mesh();
    const QString p = f.tag + QLatin1Char('.');
    const Dim force = Dim::of(1, 1, -2), torque = Dim::of(2, 1, -2);
    const PrismRule& rule = f.edges && f.edges->order() == 2 ? prismRule(4, 3) : prismRule(2, 2);
    PointEvaluator3 ev(s);
    for (const ForceCalc& fc : a_forces) {
        // Maxwell's stress tensor weighted (Arkkio's, Coulomb's virtual
        // work): F = -∫ T ∇ψ over the domains round its body, ψ 1 at the
        // body's nodes and 0 at the rest, linear in each prism - its faces'
        // integral ∮ T n moved into the first layer of elements round them,
        // the field read inside them, not at the body's edges and corners
        // (the surface's 2 % off a fed bar's Lorentz force, this 0.1 %).
        // T ∇ψ = B (H·∇ψ) - ½ (B·H) ∇ψ; time averaged ½ Re(B (H*·∇ψ)) - ¼
        // Re(B·H*) ∇ψ. The torque about its point -∫ r × (T ∇ψ).
        std::vector<char> psi(mesh.nodes.size(), 0);
        for (int e = 0; e < int(mesh.prisms.size()); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d >= 0 && d < int(fc.on.size()) && fc.on[std::size_t(d)])
                for (int n : mesh.prisms[std::size_t(e)]) psi[std::size_t(n)] = 1;
        }
        double F[3] = {0, 0, 0}, T[3] = {0, 0, 0};
        int faces = 0;
        // (Its layer's domains: Maxwell's stress is divergence-free only
        // where nothing is - no current, no magnet, µr 1, no eddy currents -,
        // else the weighted integral takes part of their own force - a bar
        // touching another fed one 12.5 % over, the review of 8d5e9e9e.)
        std::set<int> notAir;
        const Solution3::Coefficients& coef = s.coefficients()[std::size_t(a_fi)];
        const std::vector<std::array<Expression, 3>> sources = sourcesOf(-1);
        const auto air = [&](int d) {
            const auto zero = [](const Expression& e) { return !e.isValid() || (e.isConstant() && e.constant() == 0); };
            if (d < int(sources.size()))
                for (const Expression& e : sources[std::size_t(d)])
                    if (!zero(e)) return false;
            for (const char* k : {"Brx", "Bry", "Brz"})
                if (const auto it = coef.named.find(QString::fromLatin1(k)); it != coef.named.end() && d < int(it->second.size()) && !zero(it->second[std::size_t(d)]))
                    return false;
            if (d < int(coef.curves.size()) && coef.curves[std::size_t(d)]) return false;
            if (d < int(coef.c.size()))
                for (int k = 0; k < 9; ++k) {
                    const Expression& e = coef.c[std::size_t(d)][std::size_t(k)];
                    const double want = k % 4 == 0 ? 1.0 : 0.0;
                    if (e.isValid() && !(e.isConstant() && e.constant() == want)) return false;
                    if (!e.isValid() && want == 1.0) return false;
                }
            if ((harmonic || a_inTime) && conducts(d)) return false;
            return true;
        };
        for (int e = 0; e < int(mesh.prisms.size()); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || (d < int(fc.on.size()) && fc.on[std::size_t(d)]) || d >= int(f.domains.size()) || !f.domains[std::size_t(d)]) continue;
            const std::array<int, 6>& g = mesh.prisms[std::size_t(e)];
            bool touches = false;
            for (int n : g) touches = touches || psi[std::size_t(n)];
            if (!touches) continue;
            ++faces;
            if (!notAir.count(d) && !air(d)) notAir.insert(d);
            for (const auto& qp : rule.points) {
                const Space3::Map mp = sp.map(e, qp[0], qp[1], qp[2]);
                PrismShape sh;
                prismShapeAt(1, qp[0], qp[1], qp[2], sh);
                double gp[3] = {0, 0, 0};
                for (int k = 0; k < 6; ++k) {
                    if (!psi[std::size_t(g[std::size_t(k)])]) continue;
                    double gk[3];
                    Space3::gradient(mp, sh.dxi[k], sh.deta[k], sh.dzeta[k], gk);
                    for (int c = 0; c < 3; ++c) gp[c] += gk[c];
                }
                ev.at(e, qp[0], qp[1], qp[2], mp.x);
                MagneticState st;
                if (!magneticStateAt(ev, a_fi, st)) continue;
                double t[3];
                if (harmonic && st.harmonic) {
                    const double hn = st.H[0] * gp[0] + st.H[1] * gp[1] + st.H[2] * gp[2], hni = st.Hi[0] * gp[0] + st.Hi[1] * gp[1] + st.Hi[2] * gp[2];
                    const double bh = st.B[0] * st.H[0] + st.B[1] * st.H[1] + st.B[2] * st.H[2] + st.Bi[0] * st.Hi[0] + st.Bi[1] * st.Hi[1] + st.Bi[2] * st.Hi[2];
                    for (int k = 0; k < 3; ++k) t[k] = 0.5 * (st.B[k] * hn + st.Bi[k] * hni) - 0.25 * bh * gp[k];
                } else {
                    const double hn = st.H[0] * gp[0] + st.H[1] * gp[1] + st.H[2] * gp[2];
                    const double bh = st.B[0] * st.H[0] + st.B[1] * st.H[1] + st.B[2] * st.H[2];
                    for (int k = 0; k < 3; ++k) t[k] = st.B[k] * hn - 0.5 * bh * gp[k];
                }
                if (!std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2])) continue;
                const double w = qp[3] * std::abs(mp.det);
                const double r[3] = {mp.x.x - fc.point.x, mp.x.y - fc.point.y, mp.x.z - fc.point.z};
                for (int k = 0; k < 3; ++k) F[k] -= w * t[k];
                T[0] -= w * (r[1] * t[2] - r[2] * t[1]);
                T[1] -= w * (r[2] * t[0] - r[0] * t[2]);
                T[2] -= w * (r[0] * t[1] - r[1] * t[0]);
            }
        }
        if (!notAir.empty()) {
            const QString said = tr("%1's Force Calculation %2: the elements round it are not air (a current, a magnet, µr not 1 or eddy currents "
                                    "in them, %3 domain(s)) - its force and torque take part of theirs: leave an air layer round it")
                                     .arg(a_node->name(), fc.name)
                                     .arg(notAir.size());
            s.log << said;
            if (!s.warnings.contains(said)) s.warnings << said;
        }
        if (faces == 0) {
            const QString said = tr("%1's Force Calculation %2 has no domain round it the field is solved in: no force").arg(a_node->name(), fc.name);
            s.log << said;
            if (!s.warnings.contains(said)) s.warnings << said;
        }
        static const char* const axes[3] = {"x", "y", "z"};
        const QString avg = harmonic ? tr(", its time average") : QString();
        for (int k = 0; k < 3; ++k) {
            s.setGlobal(p + QStringLiteral("F") + QString::fromLatin1(axes[k]) + QLatin1Char('_') + fc.name, F[k], force,
                        tr("the force on %1 (Maxwell's stress), %2 component%3").arg(fc.name, QString::fromLatin1(axes[k]), avg));
            s.setGlobal(p + QStringLiteral("T") + QString::fromLatin1(axes[k]) + QLatin1Char('_') + fc.name, T[k], torque,
                        tr("the torque on %1 about its point, %2 component%3").arg(fc.name, QString::fromLatin1(axes[k]), avg));
        }
        s.setGlobal(p + QStringLiteral("T_") + fc.name, T[0] * fc.axis.x + T[1] * fc.axis.y + T[2] * fc.axis.z, torque,
                    tr("the torque on %1 along its axis%2").arg(fc.name, avg));
    }
}

} // namespace

std::unique_ptr<Problem3> makeMagneticProblem3(Solution3& solution, int field)
{
    return std::make_unique<MagneticProblem3>(solution, field);
}

bool magneticStateAt(PointEvaluator3& ev, int f, MagneticState& out, bool currents)
{
    const Solution3& s = ev.solution();
    if (f < 0 || f >= int(s.fields().size()) || ev.prism() < 0) return false;
    const Field3& field = s.fields()[std::size_t(f)];
    const int d = ev.domain();
    if (!field.edges || d < 0 || d >= int(field.domains.size()) || !field.domains[std::size_t(d)]) return false;
    const EdgeSpace3& es = *field.edges;
    if (field.values.size() < std::size_t(es.size())) return false;
    EdgeSpace3::Basis b;
    es.basis(ev.prism(), ev.xi(), ev.eta(), ev.zeta(), b);
    const int* ed = es.dofs(ev.prism());
    for (int k = 0; k < 3; ++k) out.A[k] = out.B[k] = 0;
    for (int i = 0; i < b.count; ++i) {
        if (ed[i] < 0) continue;
        const double a = field.values[std::size_t(ed[i])];
        for (int k = 0; k < 3; ++k) {
            out.A[k] += b.v[i][k] * a;
            out.B[k] += b.c[i][k] * a;
        }
    }
    const Solution3::Coefficients& coef = s.coefficients()[std::size_t(f)];
    // The sources' current density (a coil's of turns, an External Current
    // Density's).
    for (int k = 0; k < 3 && currents; ++k) {
        static const char* const keys[3] = {"Jx", "Jy", "Jz"};
        const auto it = coef.named.find(QString::fromLatin1(keys[k]));
        out.J[k] = out.Ji[k] = 0;
        if (it != coef.named.end() && d < int(it->second.size()) && it->second[std::size_t(d)].isValid()) {
            out.J[k] = ev.eval(it->second[std::size_t(d)]);
            if (!std::isfinite(out.J[k])) out.J[k] = 0;
        }
    }
    // Solved in the frequency domain: A's imaginary part after the
    // multiplier, then V's real and imaginary parts by node (MagneticProblem3
    // ::solveHarmonic()).
    const std::size_t nE = std::size_t(es.size()), nN = field.space ? std::size_t(field.space->size()) : 0;
    const double omega = 2 * M_PI * s.frequency();
    out.harmonic = omega > 0 && nN > 0 && field.values.size() == 2 * nE + 3 * nN;
    if (out.harmonic) {
        using cd = std::complex<double>;
        for (int k = 0; k < 3; ++k) out.Ai[k] = out.Bi[k] = 0;
        for (int i = 0; i < b.count; ++i) {
            if (ed[i] < 0) continue;
            const double a = field.values[nE + nN + std::size_t(ed[i])];
            for (int k = 0; k < 3; ++k) {
                out.Ai[k] += b.v[i][k] * a;
                out.Bi[k] += b.c[i][k] * a;
            }
        }
        double mur[9];
        const std::array<Expression, 9>& c = coef.c[std::size_t(d)];
        for (int k = 0; k < 9; ++k) mur[k] = c[std::size_t(k)].isValid() ? ev.eval(c[std::size_t(k)]) : 0;
        // (A B-H curve's element: its effective µr at its nearest
        // quadrature point, and H, the energy and the losses of B there too,
        // as the solve's bisection took them - B itself the point's own: read
        // there, B̂ round an iron ring was 0.15 % off where the point's is
        // 0.04 %.)
        double Bq[3] = {out.B[0], out.B[1], out.B[2]}, Bqi[3] = {out.Bi[0], out.Bi[1], out.Bi[2]};
        if (d < int(coef.curves.size()) && coef.curves[std::size_t(d)] && field.internalStride > 0
            && field.internal.size() == s.mesh().prisms.size() * std::size_t(field.internalStride)) {
            const PrismRule& rule = es.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
            int best = 0;
            double near = 1e300;
            for (int k = 0; k < int(rule.points.size()); ++k) {
                const auto& p = rule.points[std::size_t(k)];
                const double d2 = (p[0] - ev.xi()) * (p[0] - ev.xi()) + (p[1] - ev.eta()) * (p[1] - ev.eta()) + (p[2] - ev.zeta()) * (p[2] - ev.zeta());
                if (d2 < near) {
                    near = d2;
                    best = k;
                }
            }
            const double mu = field.internal[std::size_t(ev.prism()) * std::size_t(field.internalStride) + std::size_t(best)];
            if (mu > 0) {
                for (double& v : mur) v = 0;
                mur[0] = mur[4] = mur[8] = mu;
                const auto& p = rule.points[std::size_t(best)];
                EdgeSpace3::Basis bq;
                es.basis(ev.prism(), p[0], p[1], p[2], bq);
                for (int k = 0; k < 3; ++k) Bq[k] = Bqi[k] = 0;
                for (int i = 0; i < bq.count; ++i) {
                    if (ed[i] < 0) continue;
                    const double re = field.values[std::size_t(ed[i])], im = field.values[nE + nN + std::size_t(ed[i])];
                    for (int k = 0; k < 3; ++k) {
                        Bq[k] += bq.c[i][k] * re;
                        Bqi[k] += bq.c[i][k] * im;
                    }
                }
            }
        }
        double murIm = 0;
        if (const auto it = coef.named.find(QStringLiteral("murIm")); it != coef.named.end() && d < int(it->second.size()) && it->second[std::size_t(d)].isValid()) {
            murIm = ev.eval(it->second[std::size_t(d)]);
            if (!std::isfinite(murIm)) murIm = 0;
        }
        cd nu[9];
        if (!complexReluctivity(mur, murIm, nu)) return false;
        out.energy = out.qm = 0;
        for (int i = 0; i < 3; ++i) {
            cd H = 0;
            for (int j = 0; j < 3; ++j) H += nu[3 * i + j] * cd(Bq[j], Bqi[j]);
            out.H[i] = H.real();
            out.Hi[i] = H.imag();
            // (¼ Re(B·H*), ½ Re(jω B·H*).)
            out.energy += 0.25 * (Bq[i] * out.H[i] + Bqi[i] * out.Hi[i]);
            out.qm += 0.5 * omega * (Bq[i] * out.Hi[i] - Bqi[i] * out.H[i]);
        }
        out.mur = (mur[0] + mur[4] + mur[8]) / 3;
        // In a conductor: E = -jωA - ∇V, J = σE.
        double s9[9];
        bool conducting = false;
        for (int k = 0; k < 9; ++k) {
            s9[k] = 0;
            const auto it = coef.named.find(QStringLiteral("sigma%1").arg(k));
            if (it != coef.named.end() && d < int(it->second.size()) && it->second[std::size_t(d)].isValid()) {
                s9[k] = ev.eval(it->second[std::size_t(d)]);
                if (!std::isfinite(s9[k])) s9[k] = 0;
                conducting = true;
            }
        }
        out.sigma = (s9[0] + s9[4] + s9[8]) / 3;
        for (int k = 0; k < 3; ++k) out.E[k] = out.Ei[k] = 0;
        out.q = 0;
        if (conducting && currents) {
            const Mesh3& mesh = s.mesh();
            const int e = ev.prism();
            const Space3& ns = *field.space;
            const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
            const AffineMap am = affineMap(es.planNodes(), t);
            PrismShape sh;
            prismShapeAt(ns.order(), ev.xi(), ev.eta(), ev.zeta(), sh);
            double g[18][3];
            nodalGradients(sh, am, es.z(layer + 1) - es.z(layer), g);
            const int* nd = ns.dofs(e);
            double gr[3] = {0, 0, 0}, gi[3] = {0, 0, 0};
            for (int k = 0; k < sh.count; ++k) {
                const double vr = field.values[2 * nE + nN + std::size_t(nd[k])], vi = field.values[2 * nE + 2 * nN + std::size_t(nd[k])];
                for (int m = 0; m < 3; ++m) {
                    gr[m] += g[k][m] * vr;
                    gi[m] += g[k][m] * vi;
                }
            }
            for (int k = 0; k < sh.count; ++k) {
                out.V += sh.n[k] * field.values[2 * nE + nN + std::size_t(nd[k])];
                out.Vi += sh.n[k] * field.values[2 * nE + 2 * nN + std::size_t(nd[k])];
            }
            for (int k = 0; k < 3; ++k) {
                out.E[k] = omega * out.Ai[k] - gr[k];
                out.Ei[k] = -omega * out.A[k] - gi[k];
            }
            for (int i = 0; i < 3; ++i) {
                double jr = 0, ji = 0;
                for (int j = 0; j < 3; ++j) {
                    jr += s9[3 * i + j] * out.E[j];
                    ji += s9[3 * i + j] * out.Ei[j];
                }
                out.J[i] += jr;
                out.Ji[i] += ji;
                out.q += 0.5 * (out.E[i] * jr + out.Ei[i] * ji);
            }
        }
        return true;
    }
    const std::shared_ptr<const BHCurve>& curve = d < int(coef.curves.size()) ? coef.curves[std::size_t(d)] : nullptr;
    const double bn = std::sqrt(out.B[0] * out.B[0] + out.B[1] * out.B[1] + out.B[2] * out.B[2]);
    if (curve) {
        const double nu = curve->nu(bn);
        for (int k = 0; k < 3; ++k) out.H[k] = nu * out.B[k];
        out.energy = curve->energy(bn);
        out.mur = bn > 0 ? 1 / (Mu0 * nu) : 1 / (Mu0 * curve->nu(0));
        return true;
    }
    double mur[9], nu[9];
    const std::array<Expression, 9>& c = coef.c[std::size_t(d)];
    for (int k = 0; k < 9; ++k) mur[k] = c[std::size_t(k)].isValid() ? ev.eval(c[std::size_t(k)]) : 0;
    if (!reluctivity(mur, nu)) return false;
    double Br[3] = {0, 0, 0};
    static const char* const keys[3] = {"Brx", "Bry", "Brz"};
    for (int k = 0; k < 3; ++k) {
        const auto it = coef.named.find(QString::fromLatin1(keys[k]));
        if (it != coef.named.end() && d < int(it->second.size()) && it->second[std::size_t(d)].isValid()) {
            Br[k] = ev.eval(it->second[std::size_t(d)]);
            if (!std::isfinite(Br[k])) Br[k] = 0;
        }
    }
    out.energy = 0;
    for (int i = 0; i < 3; ++i) {
        out.H[i] = 0;
        for (int j = 0; j < 3; ++j) out.H[i] += nu[3 * i + j] * (out.B[j] - Br[j]);
        out.energy += 0.5 * out.H[i] * (out.B[i] - Br[i]);
    }
    out.mur = (mur[0] + mur[4] + mur[8]) / 3;
    double diagonal = 0;
    for (int k : {0, 4, 8}) {
        const auto it = coef.named.find(QStringLiteral("sigma%1").arg(k));
        if (it != coef.named.end() && d < int(it->second.size()) && it->second[std::size_t(d)].isValid()) {
            const double v = ev.eval(it->second[std::size_t(d)]);
            if (std::isfinite(v)) diagonal += v;
        }
    }
    out.sigma = diagonal / 3;
    // Solved in time: in a conductor E = -Ȧ - ∇V, J its sources' and σE,
    // the losses σ |E|² (Ȧ and V in Field3::internal, after A, its
    // multiplier, ψ and the coils' ψ in its values).
    if (currents && field.space && field.internal.size() == nE + nN && field.values.size() > nE + nN && field.values.size() != 2 * nE + 3 * nN) {
        double s9[9];
        bool conducting = false;
        for (int k = 0; k < 9; ++k) {
            s9[k] = 0;
            const auto it = coef.named.find(QStringLiteral("sigma%1").arg(k));
            if (it != coef.named.end() && d < int(it->second.size()) && it->second[std::size_t(d)].isValid()) {
                s9[k] = ev.eval(it->second[std::size_t(d)]);
                if (!std::isfinite(s9[k])) s9[k] = 0;
                conducting = true;
            }
        }
        if (conducting) {
            const Mesh3& mesh = s.mesh();
            const int e = ev.prism();
            const Space3& ns = *field.space;
            const int t = mesh.prismTriangle[std::size_t(e)], layer = mesh.prismLayer[std::size_t(e)];
            const AffineMap am = affineMap(es.planNodes(), t);
            PrismShape sh;
            prismShapeAt(ns.order(), ev.xi(), ev.eta(), ev.zeta(), sh);
            double g[18][3];
            nodalGradients(sh, am, es.z(layer + 1) - es.z(layer), g);
            const int* nd = ns.dofs(e);
            double Ad[3] = {0, 0, 0}, gv[3] = {0, 0, 0};
            for (int i = 0; i < b.count; ++i) {
                if (ed[i] < 0) continue;
                for (int k = 0; k < 3; ++k) Ad[k] += b.v[i][k] * field.internal[std::size_t(ed[i])];
            }
            out.V = 0;
            for (int k = 0; k < sh.count; ++k) {
                const double v = field.internal[nE + std::size_t(nd[k])];
                out.V += sh.n[k] * v;
                for (int m = 0; m < 3; ++m) gv[m] += g[k][m] * v;
            }
            out.q = 0;
            for (int k = 0; k < 3; ++k) out.E[k] = -Ad[k] - gv[k];
            for (int i = 0; i < 3; ++i) {
                double j = 0;
                for (int k = 0; k < 3; ++k) j += s9[3 * i + k] * out.E[k];
                out.J[i] += j;
                out.q += out.E[i] * j;
            }
        }
    }
    return true;
}

} // namespace qucs_s::fem::detail3
