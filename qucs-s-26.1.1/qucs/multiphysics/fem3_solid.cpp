/*
 * fem3_solid.cpp - solid mechanics in 3D: linear elasticity, the
 *                  displacement (u, v, w) on a swept mesh's Lagrange prisms,
 *                  -∇·σ = f, σ = C (ε - ε0) - eᵀE, C isotropic (E, ν) or
 *                  an anisotropic crystal's turned into the model's axes,
 *                  ε0 a thermal strain α (T - Tref); constraints fixed,
 *                  prescribed or rolling on the geometry's faces, loads on
 *                  faces and in domains, the reactions where it is held
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

#include <Eigen/Dense>

#include <algorithm>
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
    void assemble(const Equations& q, Assembled& out) const;

    std::vector<Constraint> a_constraints;              // by face
    std::vector<std::vector<Load>> a_loads;             // by face
    std::vector<std::vector<std::array<Expression, 3>>> a_body;   // by domain
    double a_emax = 0;
    double a_reaction[3] = {0, 0, 0};
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
    QStringList keys = {QStringLiteral("E"), QStringLiteral("nu"), QStringLiteral("alpha"), QStringLiteral("dT"), QStringLiteral("rho")};
    for (int I = 1; I <= 6; ++I) {
        for (int J = 1; J <= 6; ++J) keys << QStringLiteral("C%1%2").arg(I).arg(J);
        keys << QStringLiteral("alpha%1").arg(I) << QStringLiteral("sp%1").arg(I);
    }
    for (const QString& k : keys) coef.named[k].assign(D, Expression());
    a_constraints.assign(F, Constraint());
    a_loads.assign(F, {});
    a_body.assign(D, {});
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

void SolidProblem3::assemble(const Equations& q, Assembled& out) const
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
    const auto assemblePart = [this, &parts, &sp, &mesh, &f, &mats, &rule, &scatter, E, partCount, per, m](int k) {
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
            const Material& mat = mats[std::size_t(d)];
            const std::vector<std::array<Expression, 3>>& body = a_body[std::size_t(d)];
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
    // The faces: loads, and an oblique roller's penalty along its normal.
    Part faces;
    std::vector<T> penalty;
    PointEvaluator3 ev(a_s);
    const int degree = sp.order() == 1 ? 2 : 4;
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
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
    if (!sys.factorize(a.A, o.solver, 1, o.tolerance, name, &a_s.log, error)) return false;
    if (o.cancelled && o.cancelled()) {
        *error = tr("cancelled");
        return false;
    }
    if (!sys.solve(b, x, name, &a_s.log, error)) return false;
    f.values.assign(std::size_t(3 * n), 0.0);
    for (std::size_t k = 0; k < q.eq.size(); ++k) {
        if (q.eq[k] >= 0) f.values[k] = x(q.eq[k]);
        else if (q.fixed[k] >= 0) f.values[k] = ud(q.fixed[k]);
    }
    // The reactions: what holds it, by component - the fixed dofs' rows,
    // and an oblique roller's springs.
    for (double& r : a_reaction) r = 0;
    if (!q.fixedKey.empty()) {
        const Eigen::VectorXd r = a.R * x + a.Rd * ud - a.Fd;
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

// ------------------------------------------------------- Eigenfrequencies

bool SolidProblem3::eigenmodes(const Options3& o, int count, double shift, std::vector<double>* frequencies,
                               std::vector<std::vector<double>>* modes, QString* error)
{
    Field3& f = field();
    const Space3& sp = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const int n = sp.size();
    const QString name = a_node->name();
    Equations q;
    buildEquations(q);
    if (q.neq == 0) {
        *error = tr("%1 has nothing free to move").arg(name);
        return false;
    }
    // Its stiffness (what holds it: those dofs nought), and its consistent
    // mass ∫ ρ φi φj, each component's own.
    Assembled a;
    assemble(q, a);
    const Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const auto rhoIt = coef.named.find(QStringLiteral("rho"));
    using T = Eigen::Triplet<double>;
    std::vector<T> tm;
    {
        PointEvaluator3 ev(a_s);
        const PrismRule& rule = sp.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
        const int per = sp.perElement();
        std::vector<double> Me(std::size_t(per * per));
        for (int e = 0; e < sp.elements(); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            const Expression rho = rhoIt != coef.named.end() ? rhoIt->second[std::size_t(d)] : Expression();
            if (!rho.isValid()) {
                const Topology3& topo = a_s.topology();
                *error = tr("%1: domain %2 (%3) has no density rho: an Eigenfrequency step needs it")
                             .arg(name)
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
                const double w = qp[3] * std::abs(mp.det) * (std::isfinite(r) ? r : 0);
                for (int i = 0; i < per; ++i)
                    for (int j = 0; j < per; ++j) Me[std::size_t(i * per + j)] += w * s.n[i] * s.n[j];
            }
            const int* dofs = sp.dofs(e);
            for (int c = 0; c < 3; ++c)
                for (int i = 0; i < per; ++i) {
                    const int ei = q.eq[std::size_t(c * n + dofs[i])];
                    if (ei < 0) continue;
                    for (int j = 0; j < per; ++j) {
                        const int ej = q.eq[std::size_t(c * n + dofs[j])];
                        if (ej >= 0) tm.emplace_back(ei, ej, Me[std::size_t(i * per + j)]);
                    }
                }
        }
    }
    Eigen::SparseMatrix<double> M(q.neq, q.neq);
    M.setFromTriplets(tm.begin(), tm.end());
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
        frequencies->push_back(std::sqrt(std::max(md.lambda, 0.0)) / (2 * M_PI));
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
    for (int i = 0; i < n; ++i) {
        if (!on[std::size_t(i)] || f.values.size() < std::size_t(3 * n)) continue;
        const double u = f.values[std::size_t(i)], v = f.values[std::size_t(n + i)], w = f.values[std::size_t(2 * n + i)];
        dmax = std::max(dmax, std::sqrt(u * u + v * v + w * w));
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

bool solidStateAt(PointEvaluator3& ev, int f, SolidState& out)
{
    const Solution3& s = ev.solution();
    if (f < 0 || f >= int(s.fields().size())) return false;
    const int d = ev.domain();
    const Solution3::Coefficients& coef = s.coefficients()[std::size_t(f)];
    double G[3][3];
    for (int c = 0; c < 3; ++c) {
        double u = 0;
        if (!ev.field(f, c, u, G[c])) return false;
    }
    const Material m = materialOf(coef, d);
    if (!m.valid) return false;
    double C6[6][6], S0[6], SP[6];
    m.eval(&ev, C6, S0, SP);
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
