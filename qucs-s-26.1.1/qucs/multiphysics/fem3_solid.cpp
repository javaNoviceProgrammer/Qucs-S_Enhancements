/*
 * fem3_solid.cpp - solid mechanics in 3D: linear elasticity, the
 *                  displacement (u, v, w) on a swept mesh's Lagrange prisms,
 *                  -∇·σ = f, σ = C (ε - ε0) - eᵀE, C isotropic (E, ν) or
 *                  an anisotropic crystal's turned into the model's axes,
 *                  ε0 a thermal strain α (T - Tref); constraints fixed,
 *                  prescribed or rolling on the geometry's faces, loads on
 *                  faces and in domains, the reactions where it is held;
 *                  its eigenfrequencies, and its harmonic response in a
 *                  Frequency Domain step - (K (1 + jη) + jω (αM + βK) -
 *                  ω² M) u = F, a complex symmetric system
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_problem.h"

#include "fem_edge.h"
#include "fem_problem.h"
#include "fem_tensor.h"

#include "fem_sparse.h"

#include <Eigen/Dense>

#include <algorithm>
#include <complex>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>

namespace qucs_s::fem::detail3 {

namespace {

const double NaN = std::numeric_limits<double>::quiet_NaN();

/// A face's own constraint: the last feature's that holds it.
struct Constraint {
    enum Kind { None, Fixed, Displacement, Roller };
    Kind kind = None;
    int order = -1;              // its feature's place among the physics'
    bool fix[3] = {false, false, false};
    Expression value[3];         // a prescribed displacement's, by component
};

/// A load on a face: a force per area, or a pressure pushing in.
struct Load {
    bool pressure = false;
    Expression f[3], p;
};

/// A 3D model's axes from the 2D convention's (x across, y up - a wafer's
/// normal -, z along): x the same, y the 2D -z, z the 2D y. A crystal's
/// rotation into a 3D model's axes is this times its rotation into the
/// plane's (fem3_scalar.cpp's inThreeD, as a rotation).
Rotation inThreeD(const Rotation& r)
{
    static const double M[3][3] = {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
    Rotation out;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            out.r[i][j] = 0;
            for (int k = 0; k < 3; ++k) out.r[i][j] += M[i][k] * r.r[k][j];
        }
    return out;
}

/// A domain's material as the solver reads it: each expression its own, a
/// constant's value kept.
struct Material {
    bool valid = false;
    bool isotropic = true;
    Expression E, nu;                 // isotropic
    Expression C[36];                 // anisotropic, the model's axes
    Expression alpha;                 // isotropic thermal expansion
    Expression alphas[6];             // anisotropic (xx, yy, zz, yz, xz, xy)
    bool anisotropicAlpha = false;
    Expression dT;                    // invalid: no thermal strain
    Expression sp[6];                 // a piezoelectric solid's eᵀE
    bool piezo = false;
    bool constant = true;             // C, α, dT and eᵀE are all constants
    double c0[6][6], s00[6], sp0[6];  // their values when they are

    static double at(PointEvaluator3* ev, const Expression& e)
    {
        if (!e.isValid()) return 0;
        if (e.isConstant()) return e.constant();
        return ev ? ev->eval(e) : NaN;
    }

    /// Its stiffness, free strain (engineering shears) and piezoelectric
    /// stress at \a ev's point.
    void eval(PointEvaluator3* ev, double C6[6][6], double S0[6], double SP[6]) const
    {
        if (constant) {
            std::copy(&c0[0][0], &c0[0][0] + 36, &C6[0][0]);
            std::copy(s00, s00 + 6, S0);
            std::copy(sp0, sp0 + 6, SP);
            return;
        }
        compute(ev, C6, S0, SP);
    }

    void compute(PointEvaluator3* ev, double C6[6][6], double S0[6], double SP[6]) const
    {
        for (int i = 0; i < 6; ++i)
            for (int j = 0; j < 6; ++j) C6[i][j] = 0;
        if (isotropic) {
            const double e = at(ev, E), n = at(ev, nu);
            const double lambda = e * n / ((1 + n) * (1 - 2 * n)), mu = e / (2 * (1 + n));
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) C6[i][j] = lambda;
                C6[i][i] = lambda + 2 * mu;
                C6[i + 3][i + 3] = mu;
            }
        } else {
            for (int k = 0; k < 36; ++k) C6[k / 6][k % 6] = at(ev, C[k]);
        }
        const double t = dT.isValid() ? at(ev, dT) : 0;
        for (int k = 0; k < 6; ++k) {
            const double a = !dT.isValid() ? 0 : anisotropicAlpha ? at(ev, alphas[k]) : k < 3 ? at(ev, alpha) : 0;
            S0[k] = std::isfinite(a) && std::isfinite(t) ? (k < 3 ? 1.0 : 2.0) * a * t : 0.0;
            SP[k] = piezo ? at(ev, sp[k]) : 0.0;
            if (!std::isfinite(SP[k])) SP[k] = 0;
        }
    }

    void finish()
    {
        auto c = [](const Expression& e) { return !e.isValid() || e.isConstant(); };
        constant = c(E) && c(nu) && c(alpha) && c(dT);
        for (const Expression& e : C) constant = constant && c(e);
        for (const Expression& e : alphas) constant = constant && c(e);
        for (const Expression& e : sp) constant = constant && c(e);
        if (constant) compute(nullptr, c0, s00, sp0);
    }
};

/// Domain \a d's material of field \a f's coefficients (named).
Material materialOf(const Solution3::Coefficients& coef, int d)
{
    Material m;
    auto get = [&coef, d](const QString& key) {
        const auto it = coef.named.find(key);
        if (it == coef.named.end() || d < 0 || d >= int(it->second.size())) return Expression();
        return it->second[std::size_t(d)];
    };
    m.E = get(QStringLiteral("E"));
    m.nu = get(QStringLiteral("nu"));
    m.isotropic = m.E.isValid();
    m.valid = m.isotropic && m.nu.isValid();
    if (!m.isotropic) {
        m.valid = true;
        for (int k = 0; k < 36; ++k) {
            m.C[k] = get(QStringLiteral("C%1%2").arg(k / 6 + 1).arg(k % 6 + 1));
            m.valid = m.valid && m.C[k].isValid();
        }
    }
    m.dT = get(QStringLiteral("dT"));
    m.alpha = get(QStringLiteral("alpha"));
    m.anisotropicAlpha = get(QStringLiteral("alpha1")).isValid();
    for (int k = 0; k < 6; ++k) {
        if (m.anisotropicAlpha) m.alphas[k] = get(QStringLiteral("alpha%1").arg(k + 1));
        m.sp[k] = get(QStringLiteral("sp%1").arg(k + 1));
        m.piezo = m.piezo || m.sp[k].isValid();
    }
    m.finish();
    return m;
}

/// Large deformation's material at deformation \a F: its Green-Lagrange
/// strain \a Ev (Voigt's, engineering shears), its second Piola-Kirchhoff
/// stress \a Sv (Voigt's) and its tangent \a D = dS/dE (Voigt's, of E's
/// engineering shears) - Saint Venant-Kirchhoff's, S = C E; or (\a
/// neoHookean) the compressible neo-Hookean's of λ = C12 and µ = C44, ψ = µ/2
/// (I1 - 3) - µ ln J + λ/2 (ln J)²: S = µ (I - C⁻¹) + λ ln J C⁻¹, D = λ
/// C⁻¹ ⊗ C⁻¹ + (µ - λ ln J)(C⁻¹ik C⁻¹jl + C⁻¹il C⁻¹jk). False where J is not
/// above 0 (turned inside out).
bool largeStress(bool neoHookean, const double C6[6][6], const double F[3][3], double Ev[6], double Sv[6], double D[6][6])
{
    double Cg[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) Cg[i][j] = F[0][i] * F[0][j] + F[1][i] * F[1][j] + F[2][i] * F[2][j];
    Ev[0] = 0.5 * (Cg[0][0] - 1);
    Ev[1] = 0.5 * (Cg[1][1] - 1);
    Ev[2] = 0.5 * (Cg[2][2] - 1);
    Ev[3] = Cg[1][2];
    Ev[4] = Cg[0][2];
    Ev[5] = Cg[0][1];
    if (!neoHookean) {
        for (int I = 0; I < 6; ++I) {
            Sv[I] = 0;
            for (int J = 0; J < 6; ++J) {
                Sv[I] += C6[I][J] * Ev[J];
                D[I][J] = C6[I][J];
            }
        }
        return true;
    }
    const double J = F[0][0] * (F[1][1] * F[2][2] - F[1][2] * F[2][1]) - F[0][1] * (F[1][0] * F[2][2] - F[1][2] * F[2][0])
                     + F[0][2] * (F[1][0] * F[2][1] - F[1][1] * F[2][0]);
    if (!(J > 0)) return false;
    const double lambda = C6[0][1], mu = C6[3][3], lnJ = std::log(J);
    // C⁻¹ by its cofactors (det C = J²).
    double Ci[3][3];
    const double det = J * J;
    Ci[0][0] = (Cg[1][1] * Cg[2][2] - Cg[1][2] * Cg[2][1]) / det;
    Ci[0][1] = (Cg[0][2] * Cg[2][1] - Cg[0][1] * Cg[2][2]) / det;
    Ci[0][2] = (Cg[0][1] * Cg[1][2] - Cg[0][2] * Cg[1][1]) / det;
    Ci[1][1] = (Cg[0][0] * Cg[2][2] - Cg[0][2] * Cg[2][0]) / det;
    Ci[1][2] = (Cg[0][2] * Cg[1][0] - Cg[0][0] * Cg[1][2]) / det;
    Ci[2][2] = (Cg[0][0] * Cg[1][1] - Cg[0][1] * Cg[1][0]) / det;
    Ci[1][0] = Ci[0][1];
    Ci[2][0] = Ci[0][2];
    Ci[2][1] = Ci[1][2];
    static const int vi[6] = {0, 1, 2, 1, 0, 0}, vj[6] = {0, 1, 2, 2, 2, 1};
    for (int I = 0; I < 6; ++I) {
        const int i = vi[I], j = vj[I];
        Sv[I] = mu * ((i == j ? 1.0 : 0.0) - Ci[i][j]) + lambda * lnJ * Ci[i][j];
        for (int K = 0; K < 6; ++K) {
            const int k = vi[K], l = vj[K];
            D[I][K] = lambda * Ci[i][j] * Ci[k][l] + (mu - lambda * lnJ) * (Ci[i][k] * Ci[j][l] + Ci[i][l] * Ci[j][k]);
        }
    }
    return true;
}

/// The strain-displacement matrix's columns of a shape function of gradient
/// \a g: B[I][a], Voigt's rows (xx, yy, zz, yz, xz, xy), its displacement's
/// components a.
void strainColumns(const double g[3], double B[6][3])
{
    for (int I = 0; I < 6; ++I)
        for (int a = 0; a < 3; ++a) B[I][a] = 0;
    B[0][0] = g[0];
    B[1][1] = g[1];
    B[2][2] = g[2];
    B[3][1] = g[2];
    B[3][2] = g[1];
    B[4][0] = g[2];
    B[4][2] = g[0];
    B[5][0] = g[1];
    B[5][1] = g[0];
}

} // namespace

// (The gradient of the reference coordinate that is nought on the face.)
Vec3 outwardNormal3(const Space3& sp, const Mesh3::Face& face, const Space3::FacePoint& fp)
{
    if (fp.prism < 0) return {};
    if (!face.quad) return Vec3{0, 0, fp.zeta < 0.5 ? -1.0 : 1.0};
    const Space3::Map m = sp.map(fp.prism, fp.xi, fp.eta, fp.zeta);
    const double gxi[2] = {m.invT[0][0], m.invT[1][0]}, geta[2] = {m.invT[0][1], m.invT[1][1]};
    double n[2];
    const double tol = 1e-6;
    if (std::abs(fp.eta) < tol) {
        n[0] = -geta[0];
        n[1] = -geta[1];
    } else if (std::abs(fp.xi) < tol) {
        n[0] = -gxi[0];
        n[1] = -gxi[1];
    } else {
        n[0] = gxi[0] + geta[0];
        n[1] = gxi[1] + geta[1];
    }
    const double len = std::hypot(n[0], n[1]);
    return len > 0 ? Vec3{n[0] / len, n[1] / len, 0} : Vec3{};
}

namespace {

class SolidProblem3 : public Problem3
{
public:
    using Problem3::Problem3;
    bool setup(QString* error) override;
    bool solve(const Options3& o, QString* error) override;
    void setGlobals() override;
    bool eigenmodes(const Options3& o, int count, double shift, std::vector<double>* frequencies, std::vector<std::vector<double>>* modes,
                    QString* error) override;

private:
    struct Equations {
        std::vector<int> eq;        // by component * n + dof: its equation; -1 none; -3 fixed
        std::vector<int> fixed;     // by component * n + dof: its index among the fixed, -1
        std::vector<int> fixedKey;  // by fixed index: its component * n + dof
        std::vector<int> fixedFace; // by fixed index: the face whose value it takes
        int neq = 0;
    };
    void buildEquations(Equations& q) const;
    double evalAt(const Expression& e, const Vec3& x) const;
    struct Assembled {
        Eigen::SparseMatrix<double> A, Kc, R, Rd, P;
        Eigen::VectorXd F, Fd;
    };
    /// Its stiffness and loads; \a scale (by domain) its elements'
    /// stiffness alone scaled, a domain of 0 left out - no loads, no faces;
    /// \a thermal false: no thermal strain (the frequency domain's).
    void assemble(const Equations& q, Assembled& out, const std::vector<double>* scale = nullptr, bool thermal = true) const;
    /// Its consistent mass ∫ ρ φi φj, each component's own: M among the
    /// equations, Mc theirs against the held dofs, Mr and Md the held ones'
    /// rows; \a scale (by domain) multiplying ρ, a domain of 0 left out.
    /// False and why (a domain with no density).
    struct Mass {
        Eigen::SparseMatrix<double> M, Mc, Mr, Md;
    };
    bool assembleMass(const Equations& q, Mass& out, const std::vector<double>* scale, QString* error) const;
    /// The geometric stiffness of the stress as solved (its values now):
    /// ∫ ∇φiᵀ σ0 ∇φj, each component's own, among the equations.
    Eigen::SparseMatrix<double> geometricStiffness(const Equations& q) const;
    bool solveHarmonic(const Options3& o, QString* error);
    /// Large deformation: its tangent stiffness K (material and geometric,
    /// among the equations) and internal forces ∫ Bᵀ S (the equations' and
    /// the held dofs') at displacement \a u (by component * n + dof).
    void assembleLarge(const Equations& q, const std::vector<double>& u, Eigen::SparseMatrix<double>& K, Eigen::VectorXd& fint,
                       Eigen::VectorXd& fintFixed) const;
    bool solveLarge(const Options3& o, QString* error);
    /// Contact (a Contact feature's faces): each point of a face chosen
    /// against the face of another domain facing it, found once at rest
    /// (small sliding) - its prism and reference coordinates on both sides,
    /// its weight, the other side's normal (out of it), its gap there and its
    /// penalty stiffness.
    struct ContactPoint {
        int sPrism = -1, mPrism = -1;
        double sRef[3] = {0, 0, 0}, mRef[3] = {0, 0, 0};
        double w = 0, kappa = 0, g0 = 0, size = 0;
        Vec3 n;
    };
    void buildContact();
    void contactGradient(const Equations& q, const ContactPoint& c, std::vector<std::pair<int, double>>& eq,
                         std::vector<std::pair<int, double>>& fixed) const;
    bool contactTerms(const Equations& q, const Eigen::VectorXd& x, const Eigen::VectorXd& ud, Eigen::SparseMatrix<double>& K,
                      Eigen::VectorXd& r, Eigen::VectorXd& rFixed, std::vector<char>& active, bool start) const;
    /// Its contact's total normal force (each pair counted once) and least
    /// gap, at (x, ud) and the points in contact.
    void contactResults(const Equations& q, const Eigen::VectorXd& x, const Eigen::VectorXd& ud);
    /// Each domain's damping (a Damping feature's): its loss factor η and
    /// Rayleigh's αdM (1/s) and βdK (s).
    struct Damping {
        double eta = 0, alpha = 0, beta = 0;
    };

    std::vector<Constraint> a_constraints;              // by face
    std::vector<std::vector<Load>> a_loads;             // by face
    std::vector<std::vector<std::array<Expression, 3>>> a_body;   // by domain
    std::vector<Damping> a_damping;                     // by domain
    double a_emax = 0;
    double a_reaction[3] = {0, 0, 0};
    double a_reactionIm[3] = {0, 0, 0};
    bool a_harmonic = false;   // solved in the frequency domain last
    bool a_large = false;      // large deformation asked
    bool a_neoHookean = false; // its material neo-Hookean (else Saint Venant-Kirchhoff's)
    std::vector<char> a_contactFace;                    // by face: a Contact's
    double a_contactPenalty = 100, a_contactSearch = 0; // its factor, its search distance (m; 0 automatic)
    std::vector<ContactPoint> a_contactPoints;
    double a_contactForce = 0, a_gapMin = 0;
    double a_contactReach = 0;                          // the search distance used (m)
    bool a_contact = false;
};

// ------------------------------------------------------------------ Setup

bool SolidProblem3::setup(QString* error)
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
    QStringList keys = {QStringLiteral("E"), QStringLiteral("nu"), QStringLiteral("alpha"), QStringLiteral("dT"), QStringLiteral("rho"),
                        QStringLiteral("large")};
    for (int I = 1; I <= 6; ++I) {
        for (int J = 1; J <= 6; ++J) keys << QStringLiteral("C%1%2").arg(I).arg(J);
        keys << QStringLiteral("alpha%1").arg(I) << QStringLiteral("sp%1").arg(I);
    }
    for (const QString& k : keys) coef.named[k].assign(D, Expression());
    a_constraints.assign(F, Constraint());
    a_loads.assign(F, {});
    a_body.assign(D, {});
    a_damping.assign(D, Damping());
    a_large = a_node->flag(QStringLiteral("large"));
    a_neoHookean = a_large && a_node->text(QStringLiteral("hyperelastic")) == QLatin1String("neohookean");
    a_contactFace.assign(F, 0);
    a_contact = false;
    const Scope& params = a_s.parameters().scope;
    std::vector<const Node*> elasticOf(D, nullptr), piezoOf(D, nullptr);
    int order = 0;
    for (const Node& feature : a_node->children) {
        ++order;
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k) continue;
        const QVector<int> picked = resolveSelection3(topo, model, k->selection, feature.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(feature.name(), why));
        const QString t = feature.type;
        if (t == QLatin1String("solid/elastic")) {
            for (int d : picked)
                if (d >= 0 && d < int(D)) elasticOf[std::size_t(d)] = &feature;
        } else if (t == QLatin1String("solid/piezo")) {
            for (int d : picked)
                if (d >= 0 && d < int(D)) piezoOf[std::size_t(d)] = &feature;
        } else if (t == QLatin1String("solid/contact")) {
            for (int b : picked)
                if (b >= 0 && b < int(F)) a_contactFace[std::size_t(b)] = 1;
            a_contact = true;
            const Expression pf = compile(feature.text(QStringLiteral("penalty")), feature, QStringLiteral("penalty"), false);
            if (pf.isValid()) {
                if (!pf.isConstant() || !(pf.constant() > 0)) problem(tr("%1: its penalty factor must be a constant more than 0").arg(feature.name()));
                else a_contactPenalty = pf.constant();
            }
            const QString search = feature.text(QStringLiteral("search")).trimmed();
            a_contactSearch = 0;
            if (!search.isEmpty()) {
                const Expression se = compile(search, feature, QStringLiteral("search"), false);
                if (se.isValid()) {
                    if (!se.isConstant() || !(se.constant() > 0)) problem(tr("%1: its search distance must be a constant more than 0").arg(feature.name()));
                    else a_contactSearch = se.constant();
                }
            }
        } else if (t == QLatin1String("solid/damping")) {
            // (Constants: its parameters', not the point's.)
            Damping dm;
            auto constant = [&](const QString& key, double* out) {
                const Expression e = compile(feature.text(key), feature, key, false);
                if (!e.isValid()) return;
                if (!e.isConstant()) {
                    problem(tr("%1: its %2 must be a constant").arg(feature.name(), key));
                    return;
                }
                *out = e.constant();
                if (!(*out >= 0) || !std::isfinite(*out)) problem(tr("%1: its %2 must be 0 or more").arg(feature.name(), key));
            };
            if (feature.text(QStringLiteral("kind")) == QLatin1String("rayleigh")) {
                constant(QStringLiteral("alphadM"), &dm.alpha);
                constant(QStringLiteral("betadK"), &dm.beta);
            } else {
                constant(QStringLiteral("eta"), &dm.eta);
            }
            for (int d : picked)
                if (d >= 0 && d < int(D)) a_damping[std::size_t(d)] = dm;
        } else if (t == QLatin1String("solid/bodyload")) {
            std::array<Expression, 3> b;
            static const char* keys3[3] = {"fx", "fy", "fz"};
            bool ok = true;
            for (int c = 0; c < 3; ++c) {
                const QString key = QString::fromLatin1(keys3[c]);
                const QString text = feature.text(key);
                b[std::size_t(c)] = compile(text.trimmed().isEmpty() ? QStringLiteral("0[N/m^3]") : text, feature, key);
                ok = ok && b[std::size_t(c)].isValid();
            }
            if (ok)
                for (int d : picked)
                    if (d >= 0 && d < int(D)) a_body[std::size_t(d)].push_back(b);
        } else if (t == QLatin1String("solid/fixed") || t == QLatin1String("solid/displacement") || t == QLatin1String("solid/roller")
                   || t == QLatin1String("solid/free")) {
            Constraint c;
            c.order = order;
            if (t == QLatin1String("solid/fixed")) {
                c.kind = Constraint::Fixed;
                for (int a = 0; a < 3; ++a) {
                    c.fix[a] = true;
                    c.value[a] = Expression::compile(QStringLiteral("0"), a_s.pointScope());
                }
            } else if (t == QLatin1String("solid/roller")) {
                c.kind = Constraint::Roller;
            } else if (t == QLatin1String("solid/displacement")) {
                c.kind = Constraint::Displacement;
                static const char* flags[3] = {"x", "y", "z"};
                static const char* values[3] = {"u0", "v0", "w0"};
                for (int a = 0; a < 3; ++a) {
                    c.fix[a] = feature.flag(QString::fromLatin1(flags[a]));
                    if (!c.fix[a]) continue;
                    const QString key = QString::fromLatin1(values[a]);
                    const QString text = feature.text(key);
                    c.value[a] = compile(text.trimmed().isEmpty() ? QStringLiteral("0[m]") : text, feature, key, false);
                }
                if (!c.fix[0] && !c.fix[1] && !c.fix[2]) continue;
            }
            for (int b : picked)
                if (b >= 0 && b < int(F)) a_constraints[std::size_t(b)] = c;
        } else if (t == QLatin1String("solid/load")) {
            Load l;
            l.pressure = feature.text(QStringLiteral("kind")) == QLatin1String("pressure");
            bool ok = true;
            if (l.pressure) {
                l.p = compile(feature.text(QStringLiteral("p")), feature, QStringLiteral("p"));
                ok = l.p.isValid();
            } else {
                static const char* keys3[3] = {"Fx", "Fy", "Fz"};
                for (int c = 0; c < 3; ++c) {
                    const QString key = QString::fromLatin1(keys3[c]);
                    const QString text = feature.text(key);
                    l.f[c] = compile(text.trimmed().isEmpty() ? QStringLiteral("0[N/m^2]") : text, feature, key);
                    ok = ok && l.f[c].isValid();
                }
            }
            if (ok)
                for (int b : picked)
                    if (b >= 0 && b < int(F)) a_loads[std::size_t(b)].push_back(l);
        }
    }
    a_emax = 0;
    for (int d = 0; d < int(D); ++d) {
        if (!f.domains[std::size_t(d)]) continue;
        const Node* feat = elasticOf[std::size_t(d)];
        const QString object = topo.objectNames.value(topo.domains[std::size_t(d)].owner);
        if (!feat) {
            problem(tr("domain %1 (%2) has no Linear Elastic Material").arg(d + 1).arg(object));
            continue;
        }
        const bool user = feat->text(QStringLiteral("source")) == QLatin1String("user");
        const Node* m = material[std::size_t(d)];
        if (!user && !m) {
            problem(tr("domain %1 (%2) has no material: its elastic properties are needed").arg(d + 1).arg(object));
            continue;
        }
        // (A material's crystal into the 3D model's axes.)
        Rotation rotation;
        if (!user) {
            const std::optional<Rotation> r = materialRotation(*m, params, &why);
            if (!r) {
                problem(why);
                continue;
            }
            rotation = inThreeD(*r);
        }
        auto property = [&](const QString& key, const QString& name, Expression* out) {
            const QString text = user ? feat->text(key) : detail::Problem::materialText(*m, key, &params);
            if (text.trimmed().isEmpty()) {
                problem(user ? tr("domain %1 (%2): its %3 is not given").arg(d + 1).arg(object, name)
                             : tr("domain %1 (%2): its material, %3, has no %4").arg(d + 1).arg(object, m->name(), name));
                return;
            }
            *out = compile(text, user ? *feat : *m, key);
        };
        auto put = [&coef, d](const QString& key, const Expression& e) { coef.named[key][std::size_t(d)] = e; };
        // An anisotropic stiffness: as given (the model's axes), or its
        // material's crystal's turned by its orientation.
        const QString stiffness = user ? (feat->text(QStringLiteral("anisotropy")) == QLatin1String("anisotropic") ? feat->text(QStringLiteral("C")) : QString())
                                       : detail::Problem::materialText(*m, QStringLiteral("C"), &params);
        bool anisotropic = false;
        if (!stiffness.trimmed().isEmpty()) {
            std::optional<VoigtText> c = voigtText(stiffness, QLatin1Char('C'), 6, 6, true, &why);
            if (!c) {
                problem(tr("domain %1 (%2): its stiffness: %3").arg(d + 1).arg(object, why));
                continue;
            }
            if (!user) *c = rotated(*c, rotation, QLatin1Char('C'));
            const Node& from = user ? *feat : *m;
            for (int I = 0; I < 6; ++I)
                for (int J = 0; J < 6; ++J) {
                    const QString& text = c->c[I][J];
                    const Expression e = text == QLatin1String("0") || text.trimmed().isEmpty()
                                             ? Expression::compile(QStringLiteral("0[Pa]"), a_s.pointScope())
                                             : compile(text, from, QStringLiteral("C"));
                    put(QStringLiteral("C%1%2").arg(I + 1).arg(J + 1), e);
                    if (I == J && e.isValid() && e.isConstant()) {
                        a_emax = std::max(a_emax, e.constant());
                        if (I < 3 && !(e.constant() > 0)) problem(tr("domain %1 (%2): its C%3%3 must be more than 0").arg(d + 1).arg(object).arg(I + 1));
                    }
                }
            anisotropic = true;
        } else {
            Expression E, nu;
            property(QStringLiteral("E"), tr("Young's modulus"), &E);
            property(QStringLiteral("nu"), tr("Poisson's ratio"), &nu);
            if (E.isValid() && E.isConstant() && !(E.constant() > 0)) problem(tr("domain %1: its Young's modulus must be more than 0").arg(d + 1));
            if (nu.isValid() && nu.isConstant() && !(nu.constant() > -1 && nu.constant() < 0.5))
                problem(tr("domain %1: its Poisson's ratio must be between -1 and 0.5").arg(d + 1));
            if (E.isValid() && E.isConstant()) a_emax = std::max(a_emax, E.constant());
            put(QStringLiteral("E"), E);
            put(QStringLiteral("nu"), nu);
        }
        // Its density, where it is given (a step's eigenfrequencies need it).
        {
            const QString rho = user ? feat->text(QStringLiteral("rho")) : detail::Problem::materialText(*m, QStringLiteral("rho"), &params);
            if (!rho.trimmed().isEmpty()) put(QStringLiteral("rho"), compile(rho, user ? *feat : *m, QStringLiteral("rho")));
        }
        if (feat->flag(QStringLiteral("thermal"))) {
            const QString alphaText = user ? feat->text(QStringLiteral("alpha")) : detail::Problem::materialText(*m, QStringLiteral("alpha"), &params);
            if (isTensorText(alphaText)) {
                std::optional<TensorText> t = tensorText(alphaText, &why);
                if (!t) {
                    problem(tr("domain %1 (%2): its thermal expansion: %3").arg(d + 1).arg(object, why));
                    continue;
                }
                if (!user) *t = rotated(*t, rotation);
                if (!anisotropic) {
                    problem(tr("domain %1 (%2): a thermal expansion along axes is an anisotropic solid's (give its stiffness Cij)").arg(d + 1).arg(object));
                    continue;
                }
                const int vi[6] = {0, 1, 2, 1, 0, 0}, vj[6] = {0, 1, 2, 2, 2, 1};
                for (int I = 0; I < 6; ++I) {
                    const QString c = t->c[vi[I]][vj[I]];
                    put(QStringLiteral("alpha%1").arg(I + 1), c == QLatin1String("0") ? Expression::compile(QStringLiteral("0[1/K]"), a_s.pointScope())
                                                                                     : compile(c, user ? *feat : *m, QStringLiteral("alpha")));
                }
            } else {
                Expression alpha;
                property(QStringLiteral("alpha"), tr("coefficient of thermal expansion"), &alpha);
                put(QStringLiteral("alpha"), alpha);
                if (anisotropic)
                    for (int I = 0; I < 6; ++I)
                        put(QStringLiteral("alpha%1").arg(I + 1), I < 3 ? alpha : Expression::compile(QStringLiteral("0[1/K]"), a_s.pointScope()));
            }
            const QString T = feat->text(QStringLiteral("T")), ref = feat->text(QStringLiteral("Tref"));
            const Expression te = compile(T, *feat, QStringLiteral("T"));
            const Expression re = compile(ref, *feat, QStringLiteral("Tref"), false);
            if (te.isValid() && re.isValid())
                put(QStringLiteral("dT"),
                    Expression::compile(QStringLiteral("(") + T + QStringLiteral(")-(") + ref + QLatin1Char(')'), a_s.pointScope()));
        }
        // A piezoelectric solid: its stress eᵀ E, E an electrostatics' field.
        if (const Node* pz = piezoOf[std::size_t(d)]) {
            const std::optional<VoigtText> e = piezoMatrix3(params, *pz, m, &why);
            if (!e) {
                problem(tr("domain %1 (%2): %3").arg(d + 1).arg(object, why));
                continue;
            }
            const QStringList tags = pz->list(QStringLiteral("es"));
            const QString es = tags.isEmpty() ? QStringLiteral("es") : tags.first();
            static const char* axes[3] = {"x", "y", "z"};
            for (int I = 0; I < 6; ++I) {
                QStringList terms;
                for (int k = 0; k < 3; ++k)
                    if (e->c[k][I] != QLatin1String("0") && !e->c[k][I].trimmed().isEmpty())
                        terms << QStringLiteral("(%1)*%2.E%3").arg(e->c[k][I], es, QString::fromLatin1(axes[k]));
                put(QStringLiteral("sp%1").arg(I + 1), terms.isEmpty() ? Expression::compile(QStringLiteral("0[Pa]"), a_s.pointScope())
                                                                       : compile(terms.join(QLatin1String(" + ")), *pz, QStringLiteral("e")));
            }
        }
    }
    if (a_emax == 0) a_emax = 1e11;
    // Large deformation: its strain Green-Lagrange's, said to the results; no
    // thermal strain or piezoelectric stress in it (small strain's).
    if (a_large) {
        for (int d = 0; d < int(D); ++d) {
            if (!f.domains[std::size_t(d)]) continue;
            // (1 Saint Venant-Kirchhoff's, 2 neo-Hookean: an isotropic
            // material's alone.)
            coef.named[QStringLiteral("large")][std::size_t(d)] = Expression::compile(a_neoHookean ? QStringLiteral("2") : QStringLiteral("1"), a_s.pointScope());
            const Node* e = elasticOf[std::size_t(d)];
            if (a_neoHookean && !coef.named[QStringLiteral("E")][std::size_t(d)].isValid())
                problem(tr("%1: a neo-Hookean material is an isotropic one's (E, ν) - domain %2 is anisotropic").arg(a_node->name()).arg(d + 1));
            if ((e && e->flag(QStringLiteral("thermal"))) || piezoOf[std::size_t(d)])
                problem(tr("%1: large deformation takes no thermal strain or piezoelectric stress (domain %2)").arg(a_node->name()).arg(d + 1));
        }
    }
    if (a_reads.contains(a_fi)) problem(tr("%1: a solid's properties may not read its own displacement").arg(a_node->name()));
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    return true;
}

double SolidProblem3::evalAt(const Expression& e, const Vec3& x) const
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

// ------------------------------------------------------------- Equations

void SolidProblem3::buildEquations(Equations& q) const
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const Space3& sp = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const Topology3& topo = a_s.topology();
    const int n = sp.size();
    q = Equations();
    q.eq.assign(std::size_t(3 * n), -1);
    q.fixed.assign(std::size_t(3 * n), -1);
    std::vector<int> faceOf(std::size_t(3 * n), -1);
    for (int e = 0; e < sp.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        const int* dofs = sp.dofs(e);
        for (int i = 0; i < sp.perElement(); ++i)
            for (int c = 0; c < 3; ++c) q.eq[std::size_t(c * n + dofs[i])] = -2;
    }
    // The faces held, in their features' order: what each holds (a later
    // one's value over an earlier one's where they share a dof).
    std::vector<std::pair<int, int>> held;
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
        const Constraint& c = a_constraints[std::size_t(mesh.faces[std::size_t(fi)].face)];
        if (c.kind != Constraint::None) held.emplace_back(c.order, fi);
    }
    std::stable_sort(held.begin(), held.end());
    for (const auto& [ord, fi] : held) {
        (void)ord;
        const int face = mesh.faces[std::size_t(fi)].face;
        const Constraint& c = a_constraints[std::size_t(face)];
        bool fix[3] = {c.fix[0], c.fix[1], c.fix[2]};
        if (c.kind == Constraint::Roller) {
            // A plane face across an axis: that component held (else a
            // penalty along its normal, in the assembly).
            const Topology3::Face& tf = topo.faces[std::size_t(face)];
            if (!tf.planar) continue;
            int axis = -1;
            const double nv[3] = {tf.normal.x, tf.normal.y, tf.normal.z};
            for (int a = 0; a < 3; ++a)
                if (std::abs(std::abs(nv[a]) - 1) < 1e-9) axis = a;
            if (axis < 0) continue;
            fix[axis] = true;
        }
        for (int dof : sp.faceDofs(fi)) {
            if (dof < 0) continue;
            for (int a = 0; a < 3; ++a) {
                const std::size_t k = std::size_t(a * n + dof);
                if (!fix[a] || q.eq[k] == -1) continue;
                q.eq[k] = -3;
                faceOf[k] = c.kind == Constraint::Roller ? -1 : face;
            }
        }
    }
    // The equations: each dof's three components together (a node's block,
    // as multigrid's rigid-body modes want them).
    for (int dof = 0; dof < n; ++dof)
        for (int a = 0; a < 3; ++a) {
            const std::size_t k = std::size_t(a * n + dof);
            if (q.eq[k] == -3) {
                q.fixed[k] = int(q.fixedKey.size());
                q.fixedKey.push_back(int(k));
                q.fixedFace.push_back(faceOf[k]);
            } else if (q.eq[k] == -2) {
                q.eq[k] = q.neq++;
            }
        }
}

// --------------------------------------------------------------- Assembly

void SolidProblem3::assemble(const Equations& q, Assembled& out, const std::vector<double>* scale, bool thermal) const
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const Space3& sp = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const Topology3& topo = a_s.topology();
    const Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const int n = sp.size();
    const int nfix = int(q.fixedKey.size());
    const int per = sp.perElement(), m = 3 * per;
    using T = Eigen::Triplet<double>;
    out.F = Eigen::VectorXd::Zero(q.neq);
    out.Fd = Eigen::VectorXd::Zero(nfix);
    std::vector<Material> mats(topo.domains.size());
    for (int d = 0; d < int(mats.size()); ++d)
        if (f.domains[std::size_t(d)]) mats[std::size_t(d)] = materialOf(coef, d);
    struct Part {
        std::vector<T> ta, tc, tr, td;
        std::vector<std::pair<int, double>> f, fd;
    };
    // (Local dof 3 i + a: node i's component a.)
    const auto keyOf = [n](const int* dofs, int l) { return (l % 3) * n + dofs[l / 3]; };
    const auto scatter = [&q, keyOf](Part& p, const int* dofs, int m, const std::vector<double>& K, const std::vector<double>& Fe) {
        for (int i = 0; i < m; ++i) {
            const int ki = keyOf(dofs, i);
            const int ei = q.eq[std::size_t(ki)], fi = q.fixed[std::size_t(ki)];
            if (ei >= 0) p.f.emplace_back(ei, Fe[std::size_t(i)]);
            else if (fi >= 0) p.fd.emplace_back(fi, Fe[std::size_t(i)]);
            else continue;
            for (int j = 0; j < m; ++j) {
                const double v = K[std::size_t(i * m + j)];
                if (v == 0) continue;
                const int kj = keyOf(dofs, j);
                const int ej = q.eq[std::size_t(kj)], fj = q.fixed[std::size_t(kj)];
                if (ei >= 0) {
                    if (ej >= 0) p.ta.emplace_back(ei, ej, v);
                    else if (fj >= 0) p.tc.emplace_back(ei, fj, v);
                } else {
                    if (ej >= 0) p.tr.emplace_back(fi, ej, v);
                    else if (fj >= 0) p.td.emplace_back(fi, fj, v);
                }
            }
        }
    };
    const PrismRule& rule = sp.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    const int E = sp.elements();
    const int asked = assemblyParts();
    const int partCount = std::clamp(asked > 0 ? asked : E / 2000, 1, asked > 0 ? std::max(1, E) : 64);
    std::vector<Part> parts(static_cast<std::size_t>(partCount));
    const auto assemblePart = [this, &parts, &sp, &mesh, &f, &mats, &rule, &scatter, E, partCount, per, m, scale, thermal](int k) {
        Part& part = parts[std::size_t(k)];
        const int from = int(qint64(E) * k / partCount), to = int(qint64(E) * (k + 1) / partCount);
        part.ta.reserve(std::size_t(to - from) * std::size_t(m) * std::size_t(m));
        PointEvaluator3 ev(a_s);
        std::vector<double> K(static_cast<std::size_t>(m * m)), Fe(static_cast<std::size_t>(m));
        std::vector<double> grads(static_cast<std::size_t>(per) * 3);
        std::vector<std::array<double, 18>> B(static_cast<std::size_t>(per));   // B[i][I * 3 + a]
        std::vector<std::array<double, 18>> CB(static_cast<std::size_t>(per));
        for (int e = from; e < to; ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            const double factor = scale ? (*scale)[std::size_t(d)] : 1.0;
            if (factor == 0) continue;
            const Material& mat = mats[std::size_t(d)];
            static const std::vector<std::array<Expression, 3>> noBody;
            const std::vector<std::array<Expression, 3>>& body = scale ? noBody : a_body[std::size_t(d)];
            std::fill(K.begin(), K.end(), 0.0);
            std::fill(Fe.begin(), Fe.end(), 0.0);
            const int* dofs = sp.dofs(e);
            for (const auto& qp : rule.points) {
                const Space3::Map mp = sp.map(e, qp[0], qp[1], qp[2]);
                const double w = qp[3] * std::abs(mp.det);
                PrismShape s;
                prismShapeAt(sp.order(), qp[0], qp[1], qp[2], s);
                const bool here = !mat.constant || !body.empty();
                if (here) ev.at(e, qp[0], qp[1], qp[2], mp.x);
                double C6[6][6], S0[6], SP[6];
                mat.eval(here ? &ev : nullptr, C6, S0, SP);
                if (!thermal || scale)
                    for (double& v : S0) v = 0;
                if (scale)
                    for (int I = 0; I < 6; ++I) {
                        SP[I] = 0;
                        for (int J = 0; J < 6; ++J) C6[I][J] *= factor;
                    }
                double sigma0[6];
                for (int I = 0; I < 6; ++I) {
                    sigma0[I] = SP[I];
                    for (int J = 0; J < 6; ++J) sigma0[I] += C6[I][J] * S0[J];
                }
                for (int i = 0; i < per; ++i) {
                    double* g = &grads[std::size_t(3 * i)];
                    Space3::gradient(mp, s.dxi[i], s.deta[i], s.dzeta[i], g);
                    double b[6][3];
                    strainColumns(g, b);
                    for (int I = 0; I < 6; ++I)
                        for (int a = 0; a < 3; ++a) B[std::size_t(i)][std::size_t(3 * I + a)] = b[I][a];
                    for (int I = 0; I < 6; ++I)
                        for (int a = 0; a < 3; ++a) {
                            double v = 0;
                            for (int J = 0; J < 6; ++J) v += C6[I][J] * b[J][a];
                            CB[std::size_t(i)][std::size_t(3 * I + a)] = v;
                        }
                }
                for (int i = 0; i < per; ++i)
                    for (int a = 0; a < 3; ++a) {
                        const int li = 3 * i + a;
                        double fv = 0;
                        for (int I = 0; I < 6; ++I) fv += B[std::size_t(i)][std::size_t(3 * I + a)] * sigma0[I];
                        Fe[std::size_t(li)] += w * fv;
                        for (int j = 0; j < per; ++j)
                            for (int b2 = 0; b2 < 3; ++b2) {
                                double v = 0;
                                for (int I = 0; I < 6; ++I) v += B[std::size_t(i)][std::size_t(3 * I + a)] * CB[std::size_t(j)][std::size_t(3 * I + b2)];
                                K[std::size_t(li * m + 3 * j + b2)] += w * v;
                            }
                    }
                for (const auto& bf : body) {
                    double fb[3];
                    for (int a = 0; a < 3; ++a) {
                        fb[a] = Material::at(&ev, bf[std::size_t(a)]);
                        if (!std::isfinite(fb[a])) fb[a] = 0;
                    }
                    for (int i = 0; i < per; ++i)
                        for (int a = 0; a < 3; ++a) Fe[std::size_t(3 * i + a)] += w * s.n[i] * fb[a];
                }
            }
            scatter(part, dofs, m, K, Fe);
        }
    };
    if (partCount == 1) assemblePart(0);
    else {
        const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, std::min(16, partCount));
        std::atomic<int> next{0};
        detail::Threads pool;
        for (int t = 0; t < threads; ++t)
            pool.start([&next, &assemblePart, partCount] {
                for (int k = next++; k < partCount; k = next++) assemblePart(k);
            });
        pool.join();
    }
    // The faces: loads, and an oblique roller's penalty along its normal
    // (none where the stiffness alone is scaled).
    Part faces;
    std::vector<T> penalty;
    PointEvaluator3 ev(a_s);
    const int degree = sp.order() == 1 ? 2 : 4;
    for (int fi = 0; fi < int(mesh.faces.size()) && !scale; ++fi) {
        const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];
        const int face = mf.face;
        const std::vector<Load>& loads = a_loads[std::size_t(face)];
        const Constraint& c = a_constraints[std::size_t(face)];
        const Topology3::Face& tf = topo.faces[std::size_t(face)];
        bool oblique = false;
        if (c.kind == Constraint::Roller) {
            oblique = !tf.planar;
            if (tf.planar) {
                bool axis = false;
                for (double v : {tf.normal.x, tf.normal.y, tf.normal.z}) axis = axis || std::abs(std::abs(v) - 1) < 1e-9;
                oblique = !axis;
            }
        }
        if (loads.empty() && !oblique) continue;
        const std::vector<int> fd = sp.faceDofs(fi);
        bool on = !fd.empty();
        for (int dof : fd) on = on && dof >= 0 && q.eq[std::size_t(dof)] != -1;
        if (!on) continue;
        const int side = tf.back >= 0 && f.domains[std::size_t(tf.back)] ? tf.back : tf.front;
        const std::vector<Space3::FacePoint> points = sp.facePoints(fi, degree, side);
        const int nf = int(fd.size()), mfm = 3 * nf;
        std::vector<double> Kf(static_cast<std::size_t>(mfm * mfm), 0.0), Ff(static_cast<std::size_t>(mfm), 0.0);
        double area = 0;
        for (const Space3::FacePoint& fp : points) area += fp.weight;
        const double kappa = oblique ? 1e5 * a_emax / std::max(std::sqrt(area), 1e-30) : 0.0;
        for (const Space3::FacePoint& fp : points) {
            const Vec3 normal = outwardNormal3(sp, mf, fp);
            const double nv[3] = {normal.x, normal.y, normal.z};
            if (fp.prism >= 0) ev.at(fp.prism, fp.xi, fp.eta, fp.zeta, fp.x);
            double t[3] = {0, 0, 0};
            for (const Load& l : loads) {
                if (l.pressure) {
                    const double p = Material::at(&ev, l.p);
                    if (std::isfinite(p))
                        for (int a = 0; a < 3; ++a) t[a] -= p * nv[a];
                } else {
                    for (int a = 0; a < 3; ++a) {
                        const double v = Material::at(&ev, l.f[a]);
                        if (std::isfinite(v)) t[a] += v;
                    }
                }
            }
            for (int i = 0; i < nf; ++i)
                for (int a = 0; a < 3; ++a) {
                    Ff[std::size_t(3 * i + a)] += fp.weight * fp.n[i] * t[a];
                    if (oblique)
                        for (int j = 0; j < nf; ++j)
                            for (int b = 0; b < 3; ++b)
                                Kf[std::size_t((3 * i + a) * mfm + 3 * j + b)] += fp.weight * kappa * fp.n[i] * fp.n[j] * nv[a] * nv[b];
                }
        }
        scatter(faces, fd.data(), mfm, Kf, Ff);
        if (oblique)
            for (int i = 0; i < mfm; ++i) {
                const int ei = q.eq[std::size_t(keyOf(fd.data(), i))];
                if (ei < 0) continue;
                for (int j = 0; j < mfm; ++j) {
                    const int ej = q.eq[std::size_t(keyOf(fd.data(), j))];
                    const double v = Kf[std::size_t(i * mfm + j)];
                    if (ej >= 0 && v != 0) penalty.emplace_back(ei, ej, v);
                }
            }
    }
    parts.push_back(std::move(faces));
    for (Part& p : parts) {
        for (const auto& [i, v] : p.f) out.F(i) += v;
        for (const auto& [i, v] : p.fd) out.Fd(i) += v;
    }
    const auto matrix = [&parts](Eigen::SparseMatrix<double>& into, std::vector<T> Part::*member) {
        std::size_t total = 0;
        for (const Part& p : parts) total += (p.*member).size();
        std::vector<T> all;
        all.reserve(total);
        for (Part& p : parts) {
            all.insert(all.end(), (p.*member).begin(), (p.*member).end());
            (p.*member) = {};
        }
        into.setFromTriplets(all.begin(), all.end());
    };
    out.A.resize(q.neq, q.neq);
    matrix(out.A, &Part::ta);
    out.Kc.resize(q.neq, nfix);
    matrix(out.Kc, &Part::tc);
    out.R.resize(nfix, q.neq);
    matrix(out.R, &Part::tr);
    out.Rd.resize(nfix, nfix);
    matrix(out.Rd, &Part::td);
    out.P.resize(q.neq, q.neq);
    out.P.setFromTriplets(penalty.begin(), penalty.end());
}

// ------------------------------------------------------------------ Solve

bool SolidProblem3::solve(const Options3& o, QString* error)
{
    if (o.frequency > 0) return solveHarmonic(o, error);
    if (a_large) return solveLarge(o, error);
    a_harmonic = false;
    for (double& r : a_reactionIm) r = 0;
    Field3& f = field();
    const Space3& sp = *f.space;
    const int n = sp.size();
    Equations q;
    buildEquations(q);
    const QString name = a_node->name();
    if (q.neq == 0 && q.fixedKey.empty()) {
        *error = tr("%1 has nothing to solve").arg(name);
        return false;
    }
    Eigen::VectorXd ud(Eigen::Index(q.fixedKey.size()));
    for (std::size_t k = 0; k < q.fixedKey.size(); ++k) {
        const int key = q.fixedKey[k], a = key / n, dof = key % n;
        const int face = q.fixedFace[k];
        ud(Eigen::Index(k)) = face >= 0 ? evalAt(a_constraints[std::size_t(face)].value[a], sp.point(dof)) : 0.0;
        if (!std::isfinite(ud(Eigen::Index(k)))) ud(Eigen::Index(k)) = 0;
    }
    Assembled a;
    assemble(q, a);
    Eigen::VectorXd b = a.F;
    if (ud.size() > 0) b -= a.Kc * ud;
    // (Held nowhere, or not against every rigid motion: the matrix is
    // singular - said by the solver, or here where nothing is held.)
    if (q.fixedKey.empty() && a.P.nonZeros() == 0) {
        *error = tr("%1 is held nowhere: a Fixed Constraint, a Prescribed Displacement or Rollers against every rigid motion are needed")
                     .arg(name);
        return false;
    }
    System3 sys;
    // Multigrid's near null space: the six rigid-body motions at each
    // equation's dof - translations along x, y, z and rotations about them,
    // about the free dofs' middle.
    {
        std::vector<int> keyOfEq(std::size_t(q.neq), -1);
        for (std::size_t k = 0; k < q.eq.size(); ++k)
            if (q.eq[k] >= 0) keyOfEq[std::size_t(q.eq[k])] = int(k);
        Vec3 mid;
        for (int e = 0; e < q.neq; ++e) {
            const Vec3 x = sp.point(keyOfEq[std::size_t(e)] % n);
            mid.x += x.x / q.neq;
            mid.y += x.y / q.neq;
            mid.z += x.z / q.neq;
        }
        std::vector<double> B(std::size_t(q.neq) * 6, 0.0);
        for (int e = 0; e < q.neq; ++e) {
            const int key = keyOfEq[std::size_t(e)], c = key / n;
            const Vec3 x0 = sp.point(key % n);
            const double x = x0.x - mid.x, y = x0.y - mid.y, z = x0.z - mid.z;
            double* row = &B[std::size_t(e) * 6];
            row[c] = 1;
            // About x: (0, -z, y); about y: (z, 0, -x); about z: (-y, x, 0).
            const double rx[3] = {0, -z, y}, ry[3] = {z, 0, -x}, rz[3] = {-y, x, 0};
            row[3] = rx[c];
            row[4] = ry[c];
            row[5] = rz[c];
        }
        // (Each dof's three equations together, where none is held alone:
        // multigrid aggregates them as one point.)
        bool blocks = q.neq % 3 == 0;
        for (int dof = 0; dof < n && blocks; ++dof) {
            int free = 0;
            for (int c = 0; c < 3; ++c) free += q.eq[std::size_t(c * n + dof)] >= 0 ? 1 : 0;
            blocks = free == 0 || free == 3;
        }
        sys.setNearNullspace(6, std::move(B), blocks ? 3 : 1);
    }
    Eigen::VectorXd x = Eigen::VectorXd::Zero(q.neq);
    for (std::size_t k = 0; k < q.eq.size(); ++k)
        if (q.eq[k] >= 0 && k < f.values.size()) x(q.eq[k]) = f.values[k];
    Eigen::VectorXd rcFixed = Eigen::VectorXd::Zero(Eigen::Index(q.fixedKey.size()));
    if (a_contact) buildContact();
    if (!a_contactPoints.empty()) {
        // Contact: Newton's method on its energy, ½ xᵀ A x - bᵀ x + Σ ½ κ w
        // min(g, 0)² (convex), each step's length halved till the energy
        // falls (Armijo's) - full steps alone overshoot and the points in
        // contact cycle (Hertz's contact did) -, the closest pairs taken in
        // contact at the first so that a body held by its contact alone is
        // not left free.
        std::vector<char> active;
        x.setZero();
        const auto energy = [&](const Eigen::VectorXd& at) {
            double e = 0.5 * at.dot(a.A * at) - b.dot(at);
            std::vector<std::pair<int, double>> ge, gf;
            for (const ContactPoint& c : a_contactPoints) {
                contactGradient(q, c, ge, gf);
                double g = c.g0;
                for (const auto& [i, v] : ge) g += v * at(i);
                for (const auto& [i, v] : gf) g += v * ud(i);
                if (g < 0) e += 0.5 * c.kappa * c.w * g * g;
            }
            return e;
        };
        int steps = 0;
        double scaleX = 0;
        for (;; ++steps) {
            if (steps >= 100) {
                *error = tr("%1: its contact did not settle in 100 Newton steps").arg(name);
                return false;
            }
            if (o.cancelled && o.cancelled()) {
                *error = tr("cancelled");
                return false;
            }
            Eigen::SparseMatrix<double> Kc;
            Eigen::VectorXd rc;
            const bool changed = contactTerms(q, x, ud, Kc, rc, rcFixed, active, steps == 0);
            const Eigen::VectorXd grad = a.A * x - b + rc;
            System3 csys;
            QStringList quiet;
            const Eigen::SparseMatrix<double> Ac = a.A + Kc;
            if (!csys.factorize(Ac, o.solver == QLatin1String("iterative") ? o.solver : QStringLiteral("direct"), 1, o.tolerance, name, &quiet, error)
                && !csys.factorize(Ac, QStringLiteral("direct"), 1, o.tolerance, name, &quiet, error, false))
                return false;
            Eigen::VectorXd dx = Eigen::VectorXd::Zero(q.neq);
            if (!csys.solve(-grad, dx, name, &quiet, error)) return false;
            // (The first step's direction its closest pairs': taken whole.)
            double alpha = 1;
            if (steps > 0) {
                const double e0 = energy(x), slope = grad.dot(dx);
                while (alpha > 1e-6 && energy(x + alpha * dx) > e0 + 1e-4 * alpha * slope) alpha *= 0.5;
            }
            x += alpha * dx;
            scaleX = std::max(scaleX, x.lpNorm<Eigen::Infinity>());
            // (Settled: Newton's whole step a tolerance's share of where it
            // is - the points at the contact's edge may still flip, but they
            // move nothing.)
            if (steps > 0 && dx.lpNorm<Eigen::Infinity>() <= std::max(o.tolerance, 1e-12) * std::max(scaleX, 1e-300)) break;
            (void)changed;
        }
        {
            Eigen::SparseMatrix<double> Kc;
            Eigen::VectorXd rc;
            contactTerms(q, x, ud, Kc, rc, rcFixed, active, false);
        }
        const int turns = steps + 1;
        contactResults(q, x, ud);
        a_s.log << tr("%1: contact - %2 points paired, settled after %3 solves, its force %4")
                       .arg(name)
                       .arg(a_contactPoints.size())
                       .arg(turns)
                       .arg(formatQuantity(a_contactForce, Dim::of(1, 1, -2), 5));
    } else {
        if (!sys.factorize(a.A, o.solver, 1, o.tolerance, name, &a_s.log, error)) return false;
        if (o.cancelled && o.cancelled()) {
            *error = tr("cancelled");
            return false;
        }
        if (!sys.solve(b, x, name, &a_s.log, error)) return false;
    }
    f.values.assign(std::size_t(3 * n), 0.0);
    for (std::size_t k = 0; k < q.eq.size(); ++k) {
        if (q.eq[k] >= 0) f.values[k] = x(q.eq[k]);
        else if (q.fixed[k] >= 0) f.values[k] = ud(q.fixed[k]);
    }
    // The reactions: what holds it, by component - the fixed dofs' rows,
    // and an oblique roller's springs.
    for (double& r : a_reaction) r = 0;
    if (!q.fixedKey.empty()) {
        const Eigen::VectorXd r = a.R * x + a.Rd * ud - a.Fd + rcFixed;
        for (std::size_t k = 0; k < q.fixedKey.size(); ++k) a_reaction[q.fixedKey[k] / n] += r(Eigen::Index(k));
    }
    if (a.P.nonZeros() > 0) {
        const Eigen::VectorXd r = a.P * x;
        for (std::size_t k = 0; k < q.eq.size(); ++k)
            if (q.eq[k] >= 0) a_reaction[k / std::size_t(n)] += r(q.eq[k]);
    }
    f.solved = true;
    return true;
}

// ------------------------------------------------------------------- Mass

bool SolidProblem3::assembleMass(const Equations& q, Mass& out, const std::vector<double>* scale, QString* error) const
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const Space3& sp = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const int n = sp.size();
    const int nfix = int(q.fixedKey.size());
    const Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const auto rhoIt = coef.named.find(QStringLiteral("rho"));
    using T = Eigen::Triplet<double>;
    std::vector<T> tm, tc, trows, td;
    PointEvaluator3 ev(a_s);
    const PrismRule& rule = sp.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    const int per = sp.perElement();
    std::vector<double> Me(std::size_t(per * per));
    for (int e = 0; e < sp.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        const double factor = scale ? (*scale)[std::size_t(d)] : 1.0;
        if (factor == 0) continue;
        const Expression rho = rhoIt != coef.named.end() ? rhoIt->second[std::size_t(d)] : Expression();
        if (!rho.isValid()) {
            const Topology3& topo = a_s.topology();
            *error = tr("%1: domain %2 (%3) has no density rho: its mass is needed (an Eigenfrequency or a Frequency Domain step)")
                         .arg(a_node->name())
                         .arg(d + 1)
                         .arg(topo.objectNames.value(topo.domains[std::size_t(d)].owner));
            return false;
        }
        std::fill(Me.begin(), Me.end(), 0.0);
        for (const auto& qp : rule.points) {
            const Space3::Map mp = sp.map(e, qp[0], qp[1], qp[2]);
            double r = 0;
            if (rho.isConstant()) r = rho.constant();
            else {
                ev.at(e, qp[0], qp[1], qp[2], mp.x);
                r = ev.eval(rho);
            }
            PrismShape s;
            prismShapeAt(sp.order(), qp[0], qp[1], qp[2], s);
            const double w = qp[3] * std::abs(mp.det) * (std::isfinite(r) ? r : 0) * factor;
            for (int i = 0; i < per; ++i)
                for (int j = 0; j < per; ++j) Me[std::size_t(i * per + j)] += w * s.n[i] * s.n[j];
        }
        const int* dofs = sp.dofs(e);
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < per; ++i) {
                const std::size_t ki = std::size_t(c * n + dofs[i]);
                const int ei = q.eq[ki], fi = q.fixed[ki];
                if (ei < 0 && fi < 0) continue;
                for (int j = 0; j < per; ++j) {
                    const std::size_t kj = std::size_t(c * n + dofs[j]);
                    const int ej = q.eq[kj], fj = q.fixed[kj];
                    const double v = Me[std::size_t(i * per + j)];
                    if (ei >= 0) {
                        if (ej >= 0) tm.emplace_back(ei, ej, v);
                        else if (fj >= 0) tc.emplace_back(ei, fj, v);
                    } else {
                        if (ej >= 0) trows.emplace_back(fi, ej, v);
                        else if (fj >= 0) td.emplace_back(fi, fj, v);
                    }
                }
            }
    }
    out.M.resize(q.neq, q.neq);
    out.M.setFromTriplets(tm.begin(), tm.end());
    out.Mc.resize(q.neq, nfix);
    out.Mc.setFromTriplets(tc.begin(), tc.end());
    out.Mr.resize(nfix, q.neq);
    out.Mr.setFromTriplets(trows.begin(), trows.end());
    out.Md.resize(nfix, nfix);
    out.Md.setFromTriplets(td.begin(), td.end());
    return true;
}

// ---------------------------------------------------------------- Contact

void SolidProblem3::buildContact()
{
    a_contactPoints.clear();
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const Space3& sp = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const Topology3& topo = a_s.topology();
    // Each face chosen, on its solid's side: its points (a rule's) and, as a
    // face that is pushed on, how a point of it is found - a floor's
    // triangle at its height, a wall's edge between its heights - in its
    // prism's reference coordinates.
    struct Side {
        int face = -1, domain = -1, prism = -1;
        bool floor = true;
        double zeta = 0;              // a floor's
        Vec3 c[3];                    // a floor's corners (m)
        double a[2] = {0, 0}, b[2] = {0, 0};   // a wall's (ξ, η) = a + b s
        Vec3 ea, eb;                  // a wall's edge at its foot (m)
        double z0 = 0, z1 = 0;
        Vec3 normal;                  // out of its domain
        Vec3 lo, hi;                  // its box (m)
        double size = 0;              // √ its area (m)
    };
    std::vector<Side> sides;
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
        const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];
        if (mf.face < 0 || !a_contactFace[std::size_t(mf.face)]) continue;
        const Topology3::Face& tf = topo.faces[std::size_t(mf.face)];
        for (int d : {tf.back, tf.front}) {
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            // (A face between two of its domains is inside it: no contact.)
            const int other = d == tf.back ? tf.front : tf.back;
            if (other >= 0 && f.domains[std::size_t(other)]) continue;
            const std::vector<Space3::FacePoint> pts = sp.facePoints(fi, 2, d);
            if (pts.empty() || pts.front().prism < 0 || mesh.domain[std::size_t(pts.front().prism)] != d) continue;
            Side s;
            s.face = fi;
            s.domain = d;
            s.prism = pts.front().prism;
            s.floor = !mf.quad;
            s.normal = outwardNormal3(sp, mf, pts.front());
            double area = 0;
            for (const auto& p : pts) area += p.weight;
            s.size = std::sqrt(area);
            if (s.floor) {
                s.zeta = pts.front().zeta;
                static const double ref[3][2] = {{0, 0}, {1, 0}, {0, 1}};
                for (int k = 0; k < 3; ++k) s.c[k] = sp.map(s.prism, ref[k][0], ref[k][1], s.zeta).x;
            } else {
                // (Its points' s along the edge from their place, and (ξ, η)
                // linear in it.)
                const Space3::FacePoint& p0 = pts.front();
                const Space3::FacePoint* p1 = nullptr;
                for (const auto& p : pts)
                    if (std::hypot(p.xi - p0.xi, p.eta - p0.eta) > 1e-9) p1 = &p;
                if (!p1) continue;
                // The edge's ends: where (ξ, η) runs from its corner to the
                // next, along the line through the two points.
                const double dxi = p1->xi - p0.xi, deta = p1->eta - p0.eta;
                // (On an edge of the reference triangle each point is a
                // corner's mix; take t along (ξ, η) from p0: t = 0 and 1 at
                // the triangle's edge's ends.)
                double t0 = -1e300, t1 = 1e300;
                auto clip = [&](double v0, double dv, double lo, double hi) {
                    if (std::abs(dv) < 1e-12) return;
                    double a = (lo - v0) / dv, b = (hi - v0) / dv;
                    if (a > b) std::swap(a, b);
                    t0 = std::max(t0, a);
                    t1 = std::min(t1, b);
                };
                clip(p0.xi, dxi, 0, 1);
                clip(p0.eta, deta, 0, 1);
                // (ξ + η ≤ 1.)
                if (std::abs(dxi + deta) > 1e-12) {
                    const double t = (1 - p0.xi - p0.eta) / (dxi + deta);
                    if (dxi + deta > 0) t1 = std::min(t1, t);
                    else t0 = std::max(t0, t);
                }
                s.a[0] = p0.xi + t0 * dxi;
                s.a[1] = p0.eta + t0 * deta;
                s.b[0] = (t1 - t0) * dxi;
                s.b[1] = (t1 - t0) * deta;
                s.ea = sp.map(s.prism, s.a[0], s.a[1], 0).x;
                s.eb = sp.map(s.prism, s.a[0] + s.b[0], s.a[1] + s.b[1], 0).x;
                s.z0 = s.ea.z;
                s.z1 = sp.map(s.prism, s.a[0], s.a[1], 1).x.z;
            }
            s.lo = Vec3{1e300, 1e300, 1e300};
            s.hi = Vec3{-1e300, -1e300, -1e300};
            auto grow = [&s](const Vec3& x) {
                s.lo = Vec3{std::min(s.lo.x, x.x), std::min(s.lo.y, x.y), std::min(s.lo.z, x.z)};
                s.hi = Vec3{std::max(s.hi.x, x.x), std::max(s.hi.y, x.y), std::max(s.hi.z, x.z)};
            };
            if (s.floor)
                for (const Vec3& c : s.c) grow(c);
            else {
                grow(s.ea);
                grow(s.eb);
                grow(Vec3{s.ea.x, s.ea.y, s.z1});
            }
            sides.push_back(s);
        }
    }
    if (sides.empty()) return;
    double search = a_contactSearch;
    if (!(search > 0)) {
        const Box3& b = topo.bounds;
        search = 0.05 * std::max({b.x1 - b.x0, b.y1 - b.y0, b.z1 - b.z0}) * mesh.unitScale;
    }
    a_contactReach = search;
    // A point x's place on side t: its reference coordinates in t's prism,
    // its gap along t's normal; false where it falls outside t.
    auto project = [&sp](const Side& t, const Vec3& x, double ref[3], double* gap) {
        const double tol = 1e-9;
        if (t.floor) {
            const double x0 = t.c[0].x, y0 = t.c[0].y;
            const double ax = t.c[1].x - x0, ay = t.c[1].y - y0, bx = t.c[2].x - x0, by = t.c[2].y - y0;
            const double det = ax * by - ay * bx;
            if (det == 0) return false;
            const double px = x.x - x0, py = x.y - y0;
            const double l1 = (px * by - py * bx) / det, l2 = (ax * py - ay * px) / det;
            if (l1 < -tol || l2 < -tol || l1 + l2 > 1 + tol) return false;
            ref[0] = std::clamp(l1, 0.0, 1.0);
            ref[1] = std::clamp(l2, 0.0, 1.0 - ref[0]);
            ref[2] = t.zeta;
        } else {
            const double ex = t.eb.x - t.ea.x, ey = t.eb.y - t.ea.y, len2 = ex * ex + ey * ey;
            if (len2 == 0 || t.z1 == t.z0) return false;
            const double s = ((x.x - t.ea.x) * ex + (x.y - t.ea.y) * ey) / len2, zeta = (x.z - t.z0) / (t.z1 - t.z0);
            if (s < -tol || s > 1 + tol || zeta < -tol || zeta > 1 + tol) return false;
            const double sc = std::clamp(s, 0.0, 1.0);
            ref[0] = t.a[0] + sc * t.b[0];
            ref[1] = t.a[1] + sc * t.b[1];
            ref[2] = std::clamp(zeta, 0.0, 1.0);
        }
        const Vec3 on = sp.map(t.prism, ref[0], ref[1], ref[2]).x;
        *gap = (x.x - on.x) * t.normal.x + (x.y - on.y) * t.normal.y + (x.z - on.z) * t.normal.z;
        return true;
    };
    // Each side's points against every side of another domain facing it:
    // the nearest it falls on within the search distance (both ways - each
    // side pushes on the other).
    for (const Side& s : sides) {
        const std::vector<Space3::FacePoint> pts = sp.facePoints(s.face, sp.order() == 1 ? 2 : 4, s.domain);
        for (const Space3::FacePoint& p : pts) {
            if (p.prism < 0) continue;
            bool found = false;
            ContactPoint best;
            double bestGap = 1e300;
            for (const Side& t : sides) {
                if (t.domain == s.domain) continue;
                if (s.normal.x * t.normal.x + s.normal.y * t.normal.y + s.normal.z * t.normal.z > -0.5) continue;
                if (p.x.x < t.lo.x - search || p.x.x > t.hi.x + search || p.x.y < t.lo.y - search || p.x.y > t.hi.y + search
                    || p.x.z < t.lo.z - search || p.x.z > t.hi.z + search)
                    continue;
                double ref[3], gap = 0;
                if (!project(t, p.x, ref, &gap) || std::abs(gap) > search || std::abs(gap) >= std::abs(bestGap)) continue;
                bestGap = gap;
                best.mPrism = t.prism;
                for (int k = 0; k < 3; ++k) best.mRef[k] = ref[k];
                best.n = t.normal;
                best.size = t.size;
                found = true;
            }
            if (!found) continue;
            best.sPrism = p.prism;
            best.sRef[0] = p.xi;
            best.sRef[1] = p.eta;
            best.sRef[2] = p.zeta;
            best.w = p.weight;
            best.g0 = bestGap;
            best.kappa = a_contactPenalty * a_emax / std::max(std::min(best.size, s.size), 1e-30);
            a_contactPoints.push_back(best);
        }
    }
}

void SolidProblem3::contactGradient(const Equations& q, const ContactPoint& c, std::vector<std::pair<int, double>>& eq,
                                    std::vector<std::pair<int, double>>& fixed) const
{
    // ∂g/∂u: the slave's shape functions times n, the master's minus.
    eq.clear();
    fixed.clear();
    const Space3& sp = *a_s.fields()[std::size_t(a_fi)].space;
    const int n = sp.size();
    for (int side = 0; side < 2; ++side) {
        const int prism = side == 0 ? c.sPrism : c.mPrism;
        const double* ref = side == 0 ? c.sRef : c.mRef;
        const double sign = side == 0 ? 1.0 : -1.0;
        PrismShape s;
        prismShapeAt(sp.order(), ref[0], ref[1], ref[2], s);
        const int* dofs = sp.dofs(prism);
        const double nv[3] = {c.n.x, c.n.y, c.n.z};
        for (int i = 0; i < sp.perElement(); ++i) {
            if (s.n[i] == 0) continue;
            for (int a = 0; a < 3; ++a) {
                const double v = sign * s.n[i] * nv[a];
                if (v == 0) continue;
                const std::size_t key = std::size_t(a * n + dofs[i]);
                if (q.eq[key] >= 0) eq.emplace_back(q.eq[key], v);
                else if (q.fixed[key] >= 0) fixed.emplace_back(q.fixed[key], v);
            }
        }
    }
}

bool SolidProblem3::contactTerms(const Equations& q, const Eigen::VectorXd& x, const Eigen::VectorXd& ud, Eigen::SparseMatrix<double>& K,
                                 Eigen::VectorXd& r, Eigen::VectorXd& rFixed, std::vector<char>& active, bool start) const
{
    // At displacement (x, ud): each point's gap g = g0 + ∂g/∂u · u; where it
    // is below 0 its penalty's ½ κ w g² - forces κ w g ∂g/∂u, stiffness
    // κ w ∂g/∂u ∂g/∂uᵀ. True where the points in contact changed.
    using T = Eigen::Triplet<double>;
    std::vector<T> t;
    r = Eigen::VectorXd::Zero(q.neq);
    rFixed = Eigen::VectorXd::Zero(Eigen::Index(q.fixedKey.size()));
    bool changed = active.size() != a_contactPoints.size();
    active.resize(a_contactPoints.size(), 0);
    std::vector<std::pair<int, double>> ge, gf;
    // Each point's gap first: a change of the points in contact counted
    // only where its gap is more than a hundredth of the deepest
    // penetration - a point at the contact's edge, its gap all but nought,
    // flipping back and forth with nothing pushing it, not a change (the
    // turns cycled on Hertz's contact otherwise).
    std::vector<double> gaps(a_contactPoints.size());
    double deepest = 0;
    for (std::size_t k = 0; k < a_contactPoints.size(); ++k) {
        const ContactPoint& c = a_contactPoints[k];
        contactGradient(q, c, ge, gf);
        double g = c.g0;
        for (const auto& [i, v] : ge) g += v * x(i);
        for (const auto& [i, v] : gf) g += v * ud(i);
        gaps[k] = g;
        deepest = std::max(deepest, -g);
    }
    const double slack = 1e-2 * deepest;
    // (At the start the closest pairs alone - the least gap, and a thousandth
    // of the search distance more: a body held by its contact alone not left
    // free, nor pulled across the gaps of every pair found.)
    double closest = std::numeric_limits<double>::infinity();
    for (const ContactPoint& c : a_contactPoints) closest = std::min(closest, c.g0);
    const double startGap = closest + 1e-3 * a_contactReach;
    for (std::size_t k = 0; k < a_contactPoints.size(); ++k) {
        const ContactPoint& c = a_contactPoints[k];
        contactGradient(q, c, ge, gf);
        const double g = gaps[k];
        const char now = (start && c.g0 <= startGap) || g < 0 ? 1 : 0;
        if (now != active[k] && std::abs(g) > slack) changed = true;
        active[k] = now;
        if (!now) continue;
        const double kw = c.kappa * c.w;
        for (const auto& [i, v] : ge) {
            r(i) += kw * g * v;
            for (const auto& [j, w] : ge) t.emplace_back(i, j, kw * v * w);
        }
        for (const auto& [i, v] : gf) rFixed(i) += kw * g * v;
    }
    K.resize(q.neq, q.neq);
    K.setFromTriplets(t.begin(), t.end());
    return changed;
}

void SolidProblem3::contactResults(const Equations& q, const Eigen::VectorXd& x, const Eigen::VectorXd& ud)
{
    a_contactForce = 0;
    a_gapMin = std::numeric_limits<double>::infinity();
    std::vector<std::pair<int, double>> ge, gf;
    for (const ContactPoint& c : a_contactPoints) {
        contactGradient(q, c, ge, gf);
        double g = c.g0;
        for (const auto& [i, v] : ge) g += v * x(i);
        for (const auto& [i, v] : gf) g += v * ud(i);
        a_gapMin = std::min(a_gapMin, g);
        // (Each face's points push on the other's, both at once - springs
        // side by side: their forces added.)
        if (g < 0) a_contactForce += c.kappa * c.w * (-g);
    }
    if (!std::isfinite(a_gapMin)) a_gapMin = 0;
}

// ---------------------------------------------------- Geometric stiffness

Eigen::SparseMatrix<double> SolidProblem3::geometricStiffness(const Equations& q) const
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const Space3& sp = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const int n = sp.size();
    const int per = sp.perElement();
    const PrismRule& rule = sp.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    using T = Eigen::Triplet<double>;
    std::vector<T> t;
    PointEvaluator3 ev(a_s);
    std::vector<double> Ke(std::size_t(per * per)), grads(std::size_t(3 * per));
    for (int e = 0; e < sp.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        std::fill(Ke.begin(), Ke.end(), 0.0);
        for (const auto& qp : rule.points) {
            const Space3::Map mp = sp.map(e, qp[0], qp[1], qp[2]);
            ev.at(e, qp[0], qp[1], qp[2], mp.x);
            SolidState st;
            if (!solidStateAt(ev, a_fi, st)) continue;
            // (Voigt's xx, yy, zz, yz, xz, xy as a tensor.)
            const double* v = st.stress;
            const double S[3][3] = {{v[0], v[5], v[4]}, {v[5], v[1], v[3]}, {v[4], v[3], v[2]}};
            bool finite = true;
            for (int I = 0; I < 6; ++I) finite = finite && std::isfinite(v[I]);
            if (!finite) continue;
            PrismShape s;
            prismShapeAt(sp.order(), qp[0], qp[1], qp[2], s);
            for (int i = 0; i < per; ++i) Space3::gradient(mp, s.dxi[i], s.deta[i], s.dzeta[i], &grads[std::size_t(3 * i)]);
            const double w = qp[3] * std::abs(mp.det);
            for (int i = 0; i < per; ++i) {
                const double* gi = &grads[std::size_t(3 * i)];
                const double Sg[3] = {S[0][0] * gi[0] + S[0][1] * gi[1] + S[0][2] * gi[2], S[1][0] * gi[0] + S[1][1] * gi[1] + S[1][2] * gi[2],
                                      S[2][0] * gi[0] + S[2][1] * gi[1] + S[2][2] * gi[2]};
                for (int j = 0; j < per; ++j) {
                    const double* gj = &grads[std::size_t(3 * j)];
                    Ke[std::size_t(i * per + j)] += w * (Sg[0] * gj[0] + Sg[1] * gj[1] + Sg[2] * gj[2]);
                }
            }
        }
        const int* dofs = sp.dofs(e);
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < per; ++i) {
                const int ei = q.eq[std::size_t(c * n + dofs[i])];
                if (ei < 0) continue;
                for (int j = 0; j < per; ++j) {
                    const int ej = q.eq[std::size_t(c * n + dofs[j])];
                    if (ej >= 0 && Ke[std::size_t(i * per + j)] != 0) t.emplace_back(ei, ej, Ke[std::size_t(i * per + j)]);
                }
            }
    }
    Eigen::SparseMatrix<double> out(q.neq, q.neq);
    out.setFromTriplets(t.begin(), t.end());
    return out;
}

// ------------------------------------------------------- Large deformation

void SolidProblem3::assembleLarge(const Equations& q, const std::vector<double>& u, Eigen::SparseMatrix<double>& K, Eigen::VectorXd& fint,
                                  Eigen::VectorXd& fintFixed) const
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const Space3& sp = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const Topology3& topo = a_s.topology();
    const Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const int n = sp.size();
    const int nfix = int(q.fixedKey.size());
    const int per = sp.perElement(), m = 3 * per;
    using T = Eigen::Triplet<double>;
    std::vector<Material> mats(topo.domains.size());
    for (int d = 0; d < int(mats.size()); ++d)
        if (f.domains[std::size_t(d)]) mats[std::size_t(d)] = materialOf(coef, d);
    struct Part {
        std::vector<T> t;
        std::vector<std::pair<int, double>> f, fd;
    };
    const PrismRule& rule = sp.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
    const int E = sp.elements();
    const int asked = assemblyParts();
    const int partCount = std::clamp(asked > 0 ? asked : E / 2000, 1, asked > 0 ? std::max(1, E) : 64);
    std::vector<Part> parts(static_cast<std::size_t>(partCount));
    const auto assemblePart = [&, partCount](int k) {
        Part& part = parts[std::size_t(k)];
        const int from = int(qint64(E) * k / partCount), to = int(qint64(E) * (k + 1) / partCount);
        PointEvaluator3 ev(a_s);
        std::vector<double> Ke(static_cast<std::size_t>(m * m)), Fe(static_cast<std::size_t>(m)), grads(static_cast<std::size_t>(3 * per));
        std::vector<std::array<double, 18>> B(static_cast<std::size_t>(per)), CB(static_cast<std::size_t>(per));
        for (int e = from; e < to; ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            const Material& mat = mats[std::size_t(d)];
            std::fill(Ke.begin(), Ke.end(), 0.0);
            std::fill(Fe.begin(), Fe.end(), 0.0);
            const int* dofs = sp.dofs(e);
            for (const auto& qp : rule.points) {
                const Space3::Map mp = sp.map(e, qp[0], qp[1], qp[2]);
                const double w = qp[3] * std::abs(mp.det);
                PrismShape s;
                prismShapeAt(sp.order(), qp[0], qp[1], qp[2], s);
                if (!mat.constant) ev.at(e, qp[0], qp[1], qp[2], mp.x);
                double C6[6][6], S0[6], SP[6];
                mat.eval(mat.constant ? nullptr : &ev, C6, S0, SP);
                // F = I + ∇u (F[a][k] = δak + ∂ua/∂Xk) at the point.
                double F[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
                for (int i = 0; i < per; ++i) {
                    double* g = &grads[std::size_t(3 * i)];
                    Space3::gradient(mp, s.dxi[i], s.deta[i], s.dzeta[i], g);
                    for (int a = 0; a < 3; ++a) {
                        const double ua = u[std::size_t(a * n + dofs[i])];
                        for (int kk = 0; kk < 3; ++kk) F[a][kk] += ua * g[kk];
                    }
                }
                // Green-Lagrange's E (Voigt's, engineering shears), the
                // second Piola-Kirchhoff stress and its tangent - Saint
                // Venant-Kirchhoff's or neo-Hookean (a point turned inside
                // out: not finite, its Newton's step refused).
                double Ev[6], Sv[6], Dm[6][6];
                if (!largeStress(a_neoHookean, C6, F, Ev, Sv, Dm)) {
                    for (double& v : Sv) v = NaN;
                    for (auto& row : Dm)
                        for (double& v : row) v = 0;
                }
                const double St[3][3] = {{Sv[0], Sv[5], Sv[4]}, {Sv[5], Sv[1], Sv[3]}, {Sv[4], Sv[3], Sv[2]}};
                for (int i = 0; i < per; ++i) {
                    const double* g = &grads[std::size_t(3 * i)];
                    for (int a = 0; a < 3; ++a) {
                        double* b = &B[std::size_t(i)][0];
                        b[0 * 3 + a] = F[a][0] * g[0];
                        b[1 * 3 + a] = F[a][1] * g[1];
                        b[2 * 3 + a] = F[a][2] * g[2];
                        b[3 * 3 + a] = F[a][1] * g[2] + F[a][2] * g[1];
                        b[4 * 3 + a] = F[a][0] * g[2] + F[a][2] * g[0];
                        b[5 * 3 + a] = F[a][0] * g[1] + F[a][1] * g[0];
                    }
                    for (int I = 0; I < 6; ++I)
                        for (int a = 0; a < 3; ++a) {
                            double v = 0;
                            for (int J = 0; J < 6; ++J) v += Dm[I][J] * B[std::size_t(i)][std::size_t(3 * J + a)];
                            CB[std::size_t(i)][std::size_t(3 * I + a)] = v;
                        }
                }
                for (int i = 0; i < per; ++i) {
                    const double* gi = &grads[std::size_t(3 * i)];
                    const double Sg[3] = {St[0][0] * gi[0] + St[0][1] * gi[1] + St[0][2] * gi[2], St[1][0] * gi[0] + St[1][1] * gi[1] + St[1][2] * gi[2],
                                          St[2][0] * gi[0] + St[2][1] * gi[1] + St[2][2] * gi[2]};
                    for (int a = 0; a < 3; ++a) {
                        const int li = 3 * i + a;
                        double fv = 0;
                        for (int I = 0; I < 6; ++I) fv += B[std::size_t(i)][std::size_t(3 * I + a)] * Sv[I];
                        Fe[std::size_t(li)] += w * fv;
                        for (int j = 0; j < per; ++j) {
                            const double* gj = &grads[std::size_t(3 * j)];
                            const double geo = Sg[0] * gj[0] + Sg[1] * gj[1] + Sg[2] * gj[2];
                            for (int b2 = 0; b2 < 3; ++b2) {
                                double v = 0;
                                for (int I = 0; I < 6; ++I) v += B[std::size_t(i)][std::size_t(3 * I + a)] * CB[std::size_t(j)][std::size_t(3 * I + b2)];
                                if (a == b2) v += geo;
                                Ke[std::size_t(li * m + 3 * j + b2)] += w * v;
                            }
                        }
                    }
                }
            }
            for (int i = 0; i < m; ++i) {
                const int ki = (i % 3) * n + dofs[i / 3];
                const int ei = q.eq[std::size_t(ki)], fi = q.fixed[std::size_t(ki)];
                if (ei >= 0) part.f.emplace_back(ei, Fe[std::size_t(i)]);
                else if (fi >= 0) part.fd.emplace_back(fi, Fe[std::size_t(i)]);
                if (ei < 0) continue;
                for (int j = 0; j < m; ++j) {
                    const int kj = (j % 3) * n + dofs[j / 3];
                    const int ej = q.eq[std::size_t(kj)];
                    const double v = Ke[std::size_t(i * m + j)];
                    if (ej >= 0 && v != 0) part.t.emplace_back(ei, ej, v);
                }
            }
        }
    };
    if (partCount == 1) assemblePart(0);
    else {
        const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, std::min(16, partCount));
        std::atomic<int> next{0};
        detail::Threads pool;
        for (int t = 0; t < threads; ++t)
            pool.start([&next, &assemblePart, partCount] {
                for (int k = next++; k < partCount; k = next++) assemblePart(k);
            });
        pool.join();
    }
    fint = Eigen::VectorXd::Zero(q.neq);
    fintFixed = Eigen::VectorXd::Zero(nfix);
    std::size_t total = 0;
    for (const Part& p : parts) total += p.t.size();
    std::vector<T> all;
    all.reserve(total);
    for (Part& p : parts) {
        for (const auto& [i, v] : p.f) fint(i) += v;
        for (const auto& [i, v] : p.fd) fintFixed(i) += v;
        all.insert(all.end(), p.t.begin(), p.t.end());
        p.t = {};
    }
    K.resize(q.neq, q.neq);
    K.setFromTriplets(all.begin(), all.end());
}

bool SolidProblem3::solveLarge(const Options3& o, QString* error)
{
    Field3& f = field();
    const Space3& sp = *f.space;
    const int n = sp.size();
    const QString name = a_node->name();
    Equations q;
    buildEquations(q);
    if (q.neq == 0 && q.fixedKey.empty()) {
        *error = tr("%1 has nothing to solve").arg(name);
        return false;
    }
    Eigen::VectorXd ud(Eigen::Index(q.fixedKey.size()));
    for (std::size_t k = 0; k < q.fixedKey.size(); ++k) {
        const int key = q.fixedKey[k], a = key / n, dof = key % n;
        const int face = q.fixedFace[k];
        ud(Eigen::Index(k)) = face >= 0 ? evalAt(a_constraints[std::size_t(face)].value[a], sp.point(dof)) : 0.0;
        if (!std::isfinite(ud(Eigen::Index(k)))) ud(Eigen::Index(k)) = 0;
    }
    // Its loads (dead: the reference's area and normal) and an oblique
    // roller's springs, as at small strain.
    Assembled a;
    assemble(q, a, nullptr, false);
    if (q.fixedKey.empty() && a.P.nonZeros() == 0) {
        *error = tr("%1 is held nowhere: a Fixed Constraint, a Prescribed Displacement or Rollers against every rigid motion are needed")
                     .arg(name);
        return false;
    }
    std::vector<double> u(std::size_t(3 * n), 0.0);
    const double tol = std::max(o.tolerance, 1e-12);
    // Contact: its points found at rest, those in contact by each
    // iteration's gaps (its normals the rest's: small sliding).
    if (a_contact) buildContact();
    std::vector<char> active;
    bool contactStart = true;
    // Newton's method in load steps: the loads and held displacements
    // scaled by λ, from nought to 1; a step that does not converge halved.
    double lambda = 0, step = 1;
    int steps = 0, iterations = 0, halvings = 0;
    const QString solver = o.solver == QLatin1String("iterative") ? QStringLiteral("iterative") : QStringLiteral("direct");
    while (lambda < 1) {
        if (o.cancelled && o.cancelled()) {
            *error = tr("cancelled");
            return false;
        }
        const double target = std::min(1.0, lambda + step);
        std::vector<double> trial = u;
        for (std::size_t k = 0; k < q.fixedKey.size(); ++k) trial[std::size_t(q.fixedKey[k])] = target * ud(Eigen::Index(k));
        o.tell(tr("%1: load %2 %, Newton's method").arg(name).arg(qRound(100 * target)));
        bool converged = false;
        for (int it = 0; it < std::max(1, o.maxIterations); ++it) {
            Eigen::SparseMatrix<double> K;
            Eigen::VectorXd fi, fd;
            assembleLarge(q, trial, K, fi, fd);
            Eigen::VectorXd x(q.neq);
            for (std::size_t k = 0; k < q.eq.size(); ++k)
                if (q.eq[k] >= 0) x(q.eq[k]) = trial[k];
            Eigen::VectorXd r = fi + a.P * x - target * a.F;
            if (!a_contactPoints.empty()) {
                Eigen::SparseMatrix<double> Kc;
                Eigen::VectorXd rc, rcf;
                contactTerms(q, x, target * ud, Kc, rc, rcf, active, contactStart);
                contactStart = false;
                r += rc;
                K += Kc;
            }
            if (!r.allFinite()) break;
            if (a.P.nonZeros() > 0) K += a.P;
            System3 sys;
            QString why;
            QStringList quiet;
            if (!sys.factorize(K, solver, 1, tol, name, &quiet, &why) && !sys.factorize(K, solver, 1, tol, name, &quiet, &why, false)) break;
            Eigen::VectorXd dx = Eigen::VectorXd::Zero(q.neq);
            if (!sys.solve(-r, dx, name, &quiet, &why) || !dx.allFinite()) break;
            ++iterations;
            for (std::size_t k = 0; k < q.eq.size(); ++k)
                if (q.eq[k] >= 0) trial[k] += dx(q.eq[k]);
            // (Converged: its step a tolerance's share of where it is - and,
            // in contact, the points in contact the same at it.)
            double scale = 0;
            for (double v : trial) scale = std::max(scale, std::abs(v));
            if (dx.lpNorm<Eigen::Infinity>() <= tol * std::max(scale, 1e-300)) {
                if (!a_contactPoints.empty()) {
                    Eigen::VectorXd xn(q.neq);
                    for (std::size_t k = 0; k < q.eq.size(); ++k)
                        if (q.eq[k] >= 0) xn(q.eq[k]) = trial[k];
                    Eigen::SparseMatrix<double> Kc;
                    Eigen::VectorXd rc, rcf;
                    std::vector<char> again = active;
                    if (contactTerms(q, xn, target * ud, Kc, rc, rcf, again, false)) {
                        active = again;
                        continue;
                    }
                }
                converged = true;
                break;
            }
        }
        if (converged) {
            u = std::move(trial);
            lambda = target;
            ++steps;
            step = std::min(2 * step, 1.0);
        } else {
            step /= 2;
            ++halvings;
            if (step < 1.0 / 1024) {
                *error = tr("%1: Newton's method did not converge at %2 % of its load, its step 1/1024 - past a buckling, or a load too large "
                            "for its supports")
                             .arg(name)
                             .arg(formatNumber(100 * lambda, 4));
                return false;
            }
        }
    }
    f.values = u;
    // The reactions: the internal forces where it is held, less its loads
    // there.
    {
        Eigen::SparseMatrix<double> K;
        Eigen::VectorXd fi, fd;
        assembleLarge(q, u, K, fi, fd);
        for (double& r : a_reaction) r = 0;
        for (double& r : a_reactionIm) r = 0;
        Eigen::VectorXd rr = fd - a.Fd;
        if (!a_contactPoints.empty()) {
            Eigen::VectorXd x(q.neq);
            for (std::size_t k = 0; k < q.eq.size(); ++k)
                if (q.eq[k] >= 0) x(q.eq[k]) = u[k];
            Eigen::SparseMatrix<double> Kc;
            Eigen::VectorXd rc, rcf;
            std::vector<char> again = active;
            contactTerms(q, x, ud, Kc, rc, rcf, again, false);
            rr += rcf;
            contactResults(q, x, ud);
        }
        for (std::size_t k = 0; k < q.fixedKey.size(); ++k) a_reaction[q.fixedKey[k] / n] += rr(Eigen::Index(k));
        if (a.P.nonZeros() > 0) {
            Eigen::VectorXd x(q.neq);
            for (std::size_t k = 0; k < q.eq.size(); ++k)
                if (q.eq[k] >= 0) x(q.eq[k]) = u[k];
            const Eigen::VectorXd rp = a.P * x;
            for (std::size_t k = 0; k < q.eq.size(); ++k)
                if (q.eq[k] >= 0) a_reaction[k / std::size_t(n)] += rp(q.eq[k]);
        }
    }
    a_s.log << tr("%1: large deformation, %2 unknowns - %3 load steps (%4 halved), %5 Newton iterations")
                   .arg(name)
                   .arg(q.neq)
                   .arg(steps)
                   .arg(halvings)
                   .arg(iterations);
    a_harmonic = false;
    f.solved = true;
    return true;
}

// ------------------------------------------------------- Harmonic response

bool SolidProblem3::solveHarmonic(const Options3& o, QString* error)
{
    using cd = std::complex<double>;
    using CMatrix = Eigen::SparseMatrix<cd>;
    Field3& f = field();
    const Space3& sp = *f.space;
    const int n = sp.size();
    const QString name = a_node->name();
    const double w = 2 * M_PI * o.frequency;
    Equations q;
    buildEquations(q);
    if (q.neq == 0 && q.fixedKey.empty()) {
        *error = tr("%1 has nothing to solve").arg(name);
        return false;
    }
    if (a_contact) a_s.log << tr("%1: its Contact is a solve at rest's - not in the frequency domain (its faces free)").arg(name);
    // The held dofs' amplitudes (a prescribed displacement's: a base
    // shaken), in phase.
    Eigen::VectorXcd ud(Eigen::Index(q.fixedKey.size()));
    for (std::size_t k = 0; k < q.fixedKey.size(); ++k) {
        const int key = q.fixedKey[k], a = key / n, dof = key % n;
        const int face = q.fixedFace[k];
        double v = face >= 0 ? evalAt(a_constraints[std::size_t(face)].value[a], sp.point(dof)) : 0.0;
        if (!std::isfinite(v)) v = 0;
        ud(Eigen::Index(k)) = v;
    }
    // K (1 + jη) + jω (αM + βK) - ω² M: the stiffness, its imaginary part
    // by domain (η + ωβ), the mass, and its imaginary part (ωα).
    o.tell(tr("%1: assembling at %2").arg(name, formatQuantity(o.frequency, Dim::of(0, 0, -1))));
    Assembled a;
    assemble(q, a, nullptr, false);
    const std::size_t D = a_damping.size();
    std::vector<double> kScale(D, 0.0), mScale(D, 0.0);
    bool anyK = false, anyM = false;
    for (std::size_t d = 0; d < D; ++d) {
        kScale[d] = a_damping[d].eta + w * a_damping[d].beta;
        mScale[d] = w * a_damping[d].alpha;
        anyK = anyK || kScale[d] != 0;
        anyM = anyM || mScale[d] != 0;
    }
    Assembled ai;
    if (anyK) assemble(q, ai, &kScale);
    Mass mass, massI;
    if (!assembleMass(q, mass, nullptr, error)) return false;
    if (anyM && !assembleMass(q, massI, &mScale, error)) return false;
    const auto complexOf = [w](const Eigen::SparseMatrix<double>& k, const Eigen::SparseMatrix<double>& m, const Eigen::SparseMatrix<double>* ki,
                               const Eigen::SparseMatrix<double>* mi) {
        Eigen::SparseMatrix<double> re = k - (w * w) * m;
        CMatrix out = re.cast<cd>();
        if (ki && ki->nonZeros() > 0) out += cd(0, 1) * ki->cast<cd>();
        if (mi && mi->nonZeros() > 0) out += cd(0, 1) * mi->cast<cd>();
        return out;
    };
    const CMatrix A = complexOf(a.A, mass.M, anyK ? &ai.A : nullptr, anyM ? &massI.M : nullptr);
    const CMatrix Ac = complexOf(a.Kc, mass.Mc, anyK ? &ai.Kc : nullptr, anyM ? &massI.Mc : nullptr);
    Eigen::VectorXcd b = a.F.cast<cd>();
    if (ud.size() > 0) b -= Ac * ud;
    if (q.fixedKey.empty() && a.P.nonZeros() == 0 && !(w > 0)) {
        *error = tr("%1 is held nowhere").arg(name);
        return false;
    }
    Eigen::VectorXcd x = Eigen::VectorXcd::Zero(q.neq);
    if (q.neq > 0) {
        o.tell(tr("%1: factorizing").arg(name));
        SparseComplexLdlt ldlt;
        ldlt.compute(A);
        if (ldlt.info() != Eigen::Success) {
            *error = tr("%1: its harmonic system at %2 could not be factorized (%3) - at a resonance with no damping it is singular")
                         .arg(name, formatQuantity(o.frequency, Dim::of(0, 0, -1)), QString::fromStdString(ldlt.lastErrorMessage()));
            return false;
        }
        if (o.cancelled && o.cancelled()) {
            *error = tr("cancelled");
            return false;
        }
        x = ldlt.solve(b);
        // (A resonance undamped: the factors' solve not finite, or not the
        // system's.)
        const double rb = (A * x - b).norm(), nb = b.norm();
        if (!x.allFinite() || !(rb <= 1e-6 * std::max(nb, 1e-300))) {
            *error = tr("%1: its harmonic system at %2 is singular to rounding - a resonance with no damping (give a Damping)")
                         .arg(name, formatQuantity(o.frequency, Dim::of(0, 0, -1)));
            return false;
        }
        a_s.log << tr("%1: %2 unknowns at %3, by %4").arg(name).arg(q.neq).arg(formatQuantity(o.frequency, Dim::of(0, 0, -1)), ldlt.name());
    }
    // Its values: the real parts by component, then the imaginary ones.
    f.values.assign(std::size_t(6 * n), 0.0);
    for (std::size_t k = 0; k < q.eq.size(); ++k) {
        cd v = 0;
        if (q.eq[k] >= 0) v = x(q.eq[k]);
        else if (q.fixed[k] >= 0) v = ud(q.fixed[k]);
        f.values[k] = v.real();
        f.values[k + std::size_t(3 * n)] = v.imag();
    }
    // The reactions, in phase and in quadrature: the held dofs' rows of the
    // whole system.
    for (double& r : a_reaction) r = 0;
    for (double& r : a_reactionIm) r = 0;
    if (!q.fixedKey.empty()) {
        const CMatrix R = complexOf(a.R, mass.Mr, anyK ? &ai.R : nullptr, anyM ? &massI.Mr : nullptr);
        const CMatrix Rd = complexOf(a.Rd, mass.Md, anyK ? &ai.Rd : nullptr, anyM ? &massI.Md : nullptr);
        const Eigen::VectorXcd r = R * x + Rd * ud - a.Fd.cast<cd>();
        for (std::size_t k = 0; k < q.fixedKey.size(); ++k) {
            a_reaction[q.fixedKey[k] / n] += r(Eigen::Index(k)).real();
            a_reactionIm[q.fixedKey[k] / n] += r(Eigen::Index(k)).imag();
        }
    }
    if (a.P.nonZeros() > 0) {
        const Eigen::VectorXcd r = a.P.cast<cd>() * x;
        for (std::size_t k = 0; k < q.eq.size(); ++k)
            if (q.eq[k] >= 0) {
                a_reaction[k / std::size_t(n)] += r(q.eq[k]).real();
                a_reactionIm[k / std::size_t(n)] += r(q.eq[k]).imag();
            }
    }
    a_harmonic = true;
    f.solved = true;
    return true;
}

// ------------------------------------------------------- Eigenfrequencies

bool SolidProblem3::eigenmodes(const Options3& o, int count, double shift, std::vector<double>* frequencies,
                               std::vector<std::vector<double>>* modes, QString* error)
{
    Field3& f = field();
    const Space3& sp = *f.space;
    const int n = sp.size();
    const QString name = a_node->name();
    Equations q;
    buildEquations(q);
    if (q.neq == 0) {
        *error = tr("%1 has nothing free to move").arg(name);
        return false;
    }
    if (a_contact) a_s.log << tr("%1: its Contact is a solve at rest's - not in its modes (its faces free)").arg(name);
    // Its stiffness (what holds it: those dofs nought), and its consistent
    // mass ∫ ρ φi φj, each component's own.
    Assembled a;
    assemble(q, a);
    if (o.prestress) {
        // The geometric stiffness of the stress as solved: ∫ ∇φiᵀ σ0 ∇φj,
        // each component's own.
        if (f.values.size() < std::size_t(3 * n)) {
            *error = tr("%1: prestressed, but not solved at rest first").arg(name);
            return false;
        }
        if (a_large) {
            // Large deformation: the tangent about where it was solved,
            // assembleLarge()'s - its material part at F and its geometric
            // part of S -, and an oblique roller's springs.
            Eigen::SparseMatrix<double> K;
            Eigen::VectorXd fi, fd;
            const std::vector<double> at(f.values.begin(), f.values.begin() + 3 * n);
            assembleLarge(q, at, K, fi, fd);
            a.A = K + a.P;
        } else {
            a.A += geometricStiffness(q);
        }
    }
    Mass mass;
    if (!assembleMass(q, mass, nullptr, error)) return false;
    const Eigen::SparseMatrix<double>& M = mass.M;
    // Shift and invert about σ = (2π f)² - 1 s⁻² (a free solid's rigid
    // motions, at nought, then not singular): (K - σ M)⁻¹ M, its largest
    // eigenvalues θ those of K x = λ M x nearest σ, λ = σ + 1/θ.
    const double sigma = std::pow(2 * M_PI * shift, 2) - 1.0;
    const Eigen::SparseMatrix<double> S = a.A - sigma * M;
    SparseLu<double> lu;
    lu.compute(S);
    if (lu.info() != Eigen::Success) {
        *error = tr("%1: K - σM could not be factorized (%2)").arg(name, QString::fromStdString(lu.lastErrorMessage()));
        return false;
    }
    a_s.log << tr("%1: %2 unknowns, eigenfrequencies by shift and invert about %3 (an LU)")
                   .arg(name)
                   .arg(q.neq)
                   .arg(formatQuantity(shift, Dim::of(0, 0, -1)));
    const auto apply = [&lu, &M](const Eigen::VectorXcd& x, Eigen::VectorXcd& y) {
        const Eigen::VectorXd re = lu.solve(M * x.real()), im = lu.solve(M * x.imag());
        y.resize(x.size());
        for (Eigen::Index i = 0; i < x.size(); ++i) y(i) = std::complex<double>(re(i), im(i));
    };
    const int wanted = std::clamp(count, 1, q.neq);
    const EigenResult r = largestEigenvalues(q.neq, wanted, apply, 0, std::min(1e-10, o.tolerance), 300, o.cancelled);
    if (r.cancelled) {
        *error = tr("cancelled");
        return false;
    }
    if (r.values.empty()) {
        *error = tr("%1: no eigenfrequency converged%2").arg(name, r.error.isEmpty() ? QString() : QStringLiteral(" (") + r.error + QLatin1Char(')'));
        return false;
    }
    struct Mode {
        double lambda;
        int index;
    };
    std::vector<Mode> found;
    for (int k = 0; k < int(r.values.size()); ++k) {
        const std::complex<double> theta = r.values[std::size_t(k)];
        if (std::abs(theta) == 0) continue;
        found.push_back({sigma + (1.0 / theta).real(), k});
    }
    std::sort(found.begin(), found.end(), [](const Mode& x, const Mode& y) { return x.lambda < y.lambda; });
    frequencies->clear();
    modes->clear();
    for (const Mode& md : found) {
        const Eigen::VectorXcd& v = r.vectors[std::size_t(md.index)];
        // (Its phase that of its largest entry: a real mode.)
        Eigen::Index big = 0;
        for (Eigen::Index i = 0; i < v.size(); ++i)
            if (std::abs(v(i)) > std::abs(v(big))) big = i;
        const std::complex<double> phase = std::abs(v(big)) > 0 ? std::conj(v(big)) / std::abs(v(big)) : 1.0;
        std::vector<double> values(std::size_t(3 * n), 0.0);
        double most = 0;
        for (std::size_t k = 0; k < q.eq.size(); ++k)
            if (q.eq[k] >= 0) {
                values[k] = (v(q.eq[k]) * phase).real();
                most = std::max(most, std::abs(values[k]));
            }
        if (most > 0)
            for (double& x : values) x /= most;
        // (ω² below 0: buckled - said by its sign.)
        frequencies->push_back((md.lambda < 0 ? -1.0 : 1.0) * std::sqrt(std::abs(md.lambda)) / (2 * M_PI));
        modes->push_back(std::move(values));
    }
    for (double& rr : a_reaction) rr = 0;
    return true;
}

// ---------------------------------------------------------------- Globals

void SolidProblem3::setGlobals()
{
    Field3& f = field();
    const QString p = f.tag + QLatin1Char('.');
    const Space3& sp = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const int n = sp.size();
    double dmax = 0;
    std::vector<char> on(std::size_t(n), 0);
    for (int e = 0; e < sp.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        for (int i = 0; i < sp.perElement(); ++i) on[std::size_t(sp.dofs(e)[i])] = 1;
    }
    // (In the frequency domain its amplitude: the imaginary parts after
    // the real ones.)
    const bool harmonic = harmonicLayout(f);
    for (int i = 0; i < n; ++i) {
        if (!on[std::size_t(i)] || f.values.size() < std::size_t(3 * n)) continue;
        double d2 = 0;
        for (int c = 0; c < (harmonic ? 6 : 3); ++c) d2 += f.values[std::size_t(c * n + i)] * f.values[std::size_t(c * n + i)];
        dmax = std::max(dmax, std::sqrt(d2));
    }
    double energy = 0;
    {
        PointEvaluator3 ev(a_s);
        const PrismRule& rule = sp.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
        for (int e = 0; e < sp.elements(); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            for (const auto& qp : rule.points) {
                const Space3::Map mp = sp.map(e, qp[0], qp[1], qp[2]);
                ev.at(e, qp[0], qp[1], qp[2], mp.x);
                SolidState st;
                if (solidStateAt(ev, a_fi, st) && std::isfinite(st.energy)) energy += qp[3] * std::abs(mp.det) * st.energy;
            }
        }
    }
    const Dim force = Dim::of(1, 1, -2);
    a_s.setGlobal(p + QStringLiteral("dmax"), dmax, Dim::length(), tr("largest displacement"));
    a_s.setGlobal(p + QStringLiteral("W"), energy, Dim::of(2, 1, -2), tr("strain energy"));
    a_s.setGlobal(p + QStringLiteral("Rx"), a_reaction[0], force, tr("reaction force where it is held, x component"));
    a_s.setGlobal(p + QStringLiteral("Ry"), a_reaction[1], force, tr("reaction force where it is held, y component"));
    a_s.setGlobal(p + QStringLiteral("Rz"), a_reaction[2], force, tr("reaction force where it is held, z component"));
    if (a_contact) {
        a_s.setGlobal(p + QStringLiteral("Fn"), a_contactForce, force, tr("contact: its total normal force"));
        a_s.setGlobal(p + QStringLiteral("gapmin"), a_gapMin, Dim::length(), tr("contact: its least gap (below 0: interpenetrating, the penalty's)"));
    }
    if (a_harmonic) {
        static const char* axes[3] = {"x", "y", "z"};
        for (int a = 0; a < 3; ++a)
            a_s.setGlobal(p + QStringLiteral("R%1_im").arg(QString::fromLatin1(axes[a])), a_reactionIm[a], force,
                          tr("reaction force where it is held, %1 component, its imaginary part (the frequency domain's)").arg(QString::fromLatin1(axes[a])));
    }
}

} // namespace

std::optional<VoigtText> piezoMatrix3(const Scope& params, const Node& feature, const Node* m, QString* why)
{
    const bool user = feature.text(QStringLiteral("source")) == QLatin1String("user");
    Rotation rotation;
    if (!user) {
        if (!m) {
            *why = tr("it has no material: its piezoelectric constants e are needed (%1)").arg(feature.name());
            return std::nullopt;
        }
        const std::optional<Rotation> r = materialRotation(*m, params, why);
        if (!r) return std::nullopt;
        rotation = inThreeD(*r);
    }
    const QString text = user ? feature.text(QStringLiteral("e")) : detail::Problem::materialText(*m, QStringLiteral("e"), &params);
    if (!user && text.trimmed().isEmpty() && !detail::Problem::materialText(*m, QStringLiteral("d"), &params).trimmed().isEmpty()) {
        // A material's strain constants d: e = d C, its crystal's axes.
        const QString stiffness = detail::Problem::materialText(*m, QStringLiteral("C"), &params);
        if (stiffness.trimmed().isEmpty()) {
            *why = tr("its material, %1, has piezoelectric strain constants d but no stiffness Cij: e = d C (%2)").arg(m->name(), feature.name());
            return std::nullopt;
        }
        const std::optional<VoigtText> dm = voigtText(detail::Problem::materialText(*m, QStringLiteral("d"), &params), QLatin1Char('d'), 3, 6, false, why);
        if (!dm) return std::nullopt;
        const std::optional<VoigtText> cm = voigtText(stiffness, QLatin1Char('C'), 6, 6, true, why);
        if (!cm) return std::nullopt;
        return rotated(multiplied(*dm, *cm), rotation, QLatin1Char('e'));
    }
    if (text.trimmed().isEmpty()) {
        *why = user ? tr("%1: its piezoelectric constants e are not given").arg(feature.name())
                    : tr("its material, %1, has no piezoelectric constants, e or d (%2)").arg(m->name(), feature.name());
        return std::nullopt;
    }
    std::optional<VoigtText> e = voigtText(text, QLatin1Char('e'), 3, 6, false, why);
    if (e && !user) *e = rotated(*e, rotation, QLatin1Char('e'));
    return e;
}

std::unique_ptr<Problem3> makeSolidProblem3(Solution3& solution, int field)
{
    return std::make_unique<SolidProblem3>(solution, field);
}

bool solidStateAt(PointEvaluator3& ev, int f, SolidState& out, bool imaginary)
{
    const Solution3& s = ev.solution();
    if (f < 0 || f >= int(s.fields().size())) return false;
    const int d = ev.domain();
    const Solution3::Coefficients& coef = s.coefficients()[std::size_t(f)];
    double G[3][3];
    for (int c = 0; c < 3; ++c) {
        double u = 0;
        if (!ev.field(f, c + (imaginary ? 3 : 0), u, G[c])) return false;
    }
    const Material m = materialOf(coef, d);
    if (!m.valid) return false;
    double C6[6][6], S0[6], SP[6];
    m.eval(&ev, C6, S0, SP);
    if (imaginary)
        for (int I = 0; I < 6; ++I) S0[I] = SP[I] = 0;
    // Large deformation: the Green-Lagrange strain (engineering shears), its
    // energy ½ E·C E a volume at rest, and the Cauchy stress (1/J) F S Fᵀ.
    if (const auto it = coef.named.find(QStringLiteral("large")); it != coef.named.end() && d >= 0 && d < int(it->second.size()) && it->second[std::size_t(d)].isValid()) {
        double F[3][3];
        for (int a = 0; a < 3; ++a)
            for (int k = 0; k < 3; ++k) F[a][k] = (a == k ? 1.0 : 0.0) + G[a][k];
        const Expression& kind = it->second[std::size_t(d)];
        const bool neo = kind.isConstant() && kind.constant() == 2;
        double Ev[6], Sv[6], Dm[6][6];
        if (!largeStress(neo, C6, F, Ev, Sv, Dm)) return false;
        out.energy = 0;
        for (int I = 0; I < 6; ++I) {
            out.energy += 0.5 * Ev[I] * Sv[I];
            out.strain[I] = Ev[I];
        }
        if (neo) {
            // (Its stored energy ψ itself.)
            const double J = F[0][0] * (F[1][1] * F[2][2] - F[1][2] * F[2][1]) - F[0][1] * (F[1][0] * F[2][2] - F[1][2] * F[2][0])
                             + F[0][2] * (F[1][0] * F[2][1] - F[1][1] * F[2][0]);
            const double I1 = 3 + 2 * (Ev[0] + Ev[1] + Ev[2]), lambda = C6[0][1], mu = C6[3][3];
            out.energy = 0.5 * mu * (I1 - 3) - mu * std::log(J) + 0.5 * lambda * std::log(J) * std::log(J);
        }
        const double St[3][3] = {{Sv[0], Sv[5], Sv[4]}, {Sv[5], Sv[1], Sv[3]}, {Sv[4], Sv[3], Sv[2]}};
        const double J = F[0][0] * (F[1][1] * F[2][2] - F[1][2] * F[2][1]) - F[0][1] * (F[1][0] * F[2][2] - F[1][2] * F[2][0])
                         + F[0][2] * (F[1][0] * F[2][1] - F[1][1] * F[2][0]);
        double sig[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                double v = 0;
                for (int k = 0; k < 3; ++k)
                    for (int l = 0; l < 3; ++l) v += F[i][k] * St[k][l] * F[j][l];
                sig[i][j] = J != 0 ? v / J : NaN;
            }
        out.stress[0] = sig[0][0];
        out.stress[1] = sig[1][1];
        out.stress[2] = sig[2][2];
        out.stress[3] = sig[1][2];
        out.stress[4] = sig[0][2];
        out.stress[5] = sig[0][1];
        return true;
    }
    double* S = out.strain;
    S[0] = G[0][0];
    S[1] = G[1][1];
    S[2] = G[2][2];
    S[3] = G[1][2] + G[2][1];
    S[4] = G[0][2] + G[2][0];
    S[5] = G[0][1] + G[1][0];
    double el[6];
    for (int I = 0; I < 6; ++I) el[I] = S[I] - S0[I];
    out.energy = 0;
    for (int I = 0; I < 6; ++I) {
        double v = 0;
        for (int J = 0; J < 6; ++J) v += C6[I][J] * el[J];
        out.energy += 0.5 * el[I] * v;
        out.stress[I] = v - SP[I];
    }
    return true;
}

} // namespace qucs_s::fem::detail3
