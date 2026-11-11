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
 *                     and inductance matrix, the Lorentz force on each coil
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
/// terminal and the rest, scaled to N I0 through it.
struct Coil {
    QString name;
    std::vector<char> on;        // by domain
    Expression current, turns;   // I0, N
    QString ec;                  // the currents physics' tag
    QString terminal;            // its terminal whose current J carries (empty: its first)
    double perAmp = 0;           // N / I_ec, as last solved: its J at 1 A
    double scale = 0;            // N I0 / I_ec
    double flux = 0;             // its flux linkage as solved (Wb)
    double force[3] = {0, 0, 0}; // ∫ J × B over it (N)
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

class MagneticProblem3 : public Problem3
{
public:
    using Problem3::Problem3;
    bool setup(QString* error) override;
    bool solve(const Options3& o, QString* error) override;
    void setGlobals() override;

private:
    struct Equations {
        std::vector<int> eqA, eqP;     // by edge dof / node dof: its equation, -1 none, -3 fixed
        std::vector<double> fixedA;    // by edge dof: its held value (a potential's)
        int nA = 0, nP = 0;
    };
    bool buildEquations(Equations& q, QString* error);
    /// The source J by domain: the External Current Densities', and each
    /// coil's N I0 / I_ec times the currents physics' J (\a only: that coil
    /// alone at 1 A, nothing else).
    bool buildSources(QString* error);
    std::vector<std::array<Expression, 3>> sourcesOf(int only) const;
    /// Newton's system at A (\a a, by edge dof): its matrix (the tangent,
    /// and the gauge's rows and columns scaled), and the residual.
    void assemble(const Equations& q, const std::vector<double>& a, const std::vector<std::array<Expression, 3>>& J, bool magnets,
                  Eigen::SparseMatrix<double>& M, Eigen::VectorXd& r, double gaugeScale) const;
    double evalAt(const Expression& e, const Vec3& x) const;

    std::vector<FaceCondition> a_faces;                                // by face
    std::vector<std::vector<std::array<Expression, 3>>> a_sheets;      // by face
    std::vector<std::vector<std::array<Expression, 3>>> a_external;    // by domain
    std::vector<Coil> a_coils;
    bool a_nonlinear = false;
    bool a_magnets = false;
    double a_energy = 0;
    std::vector<std::vector<double>> a_inductance;   // coils swept at 1 A each: [i][j]
};

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
    for (const char* k : {"Brx", "Bry", "Brz", "Jx", "Jy", "Jz"}) coef.named[QString::fromLatin1(k)].assign(D, Expression());
    coef.curves.assign(D, nullptr);
    a_faces.assign(F, FaceCondition());
    a_sheets.assign(F, {});
    a_external.assign(D, {});
    a_coils.clear();
    a_nonlinear = false;
    a_magnets = false;
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
        } else if (t == QLatin1String("magnetic/coil")) {
            Coil c;
            c.name = feature.text(QStringLiteral("name"));
            if (c.name.trimmed().isEmpty()) c.name = feature.name();
            c.on.assign(D, 0);
            for (int d : picked)
                if (d >= 0 && d < int(D)) c.on[std::size_t(d)] = 1;
            c.current = compile(feature.text(QStringLiteral("I0")), feature, QStringLiteral("I0"), false);
            const QString n = feature.text(QStringLiteral("conductor")) == QLatin1String("multi") ? feature.text(QStringLiteral("N")) : QStringLiteral("1");
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
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    return true;
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

std::vector<std::array<Expression, 3>> MagneticProblem3::sourcesOf(int only) const
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
                if (!coil.on[d] || (only >= 0 && only != k)) continue;
                // (A coil alone at 1 A: N / I_ec times its J.)
                const double scale = only >= 0 ? coil.perAmp : coil.scale;
                terms << QStringLiteral("%1*%2.J%3").arg(scale, 0, 'g', 17).arg(coil.ec, QString::fromLatin1(axes[c]));
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
        const FaceCondition& c = a_faces[std::size_t(mesh.faces[std::size_t(fi)].face)];
        bool on = true;
        for (int dof : ns.faceDofs(fi)) on = on && dof >= 0 && q.eqP[std::size_t(dof)] != -1;
        if (!on) continue;
        for (int dof : edgeDofs(fi)) {
            if (dof < 0 || q.eqA[std::size_t(dof)] == -1) continue;
            q.eqA[std::size_t(dof)] = -3;
            if (c.kind == FaceCondition::Potential) potential.insert(dof);
            else potential.erase(dof);
        }
        for (int dof : ns.faceDofs(fi))
            if (dof >= 0) q.eqP[std::size_t(dof)] = -3;
        if (c.kind == FaceCondition::Potential) potentialFaces.push_back(fi);
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
    if (!buildSources(error)) return false;
    Equations q;
    if (!buildEquations(q, error)) return false;
    // The gauge's rows scaled to the curl-curl's: ν0 / h², h the mesh's
    // typical prism (the multiplier is nought where J is free of divergence:
    // only the pivots mind).
    double volume = 0;
    {
        const Mesh3& mesh = a_s.mesh();
        for (int e = 0; e < es.elements(); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            EdgeSpace3::Basis b;
            es.basis(e, 1.0 / 3, 1.0 / 3, 0.5, b);
            volume += 0.5 * b.det;
        }
    }
    const double hTypical = std::cbrt(std::max(volume, 1e-300) / std::max(1, es.elements()));
    const double gaugeScale = 1 / (Mu0 * hTypical * hTypical);
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
    // The saddle point is symmetric and indefinite: an LDLᵀ with pivoting
    // (Accelerate's, of a complex symmetric matrix its imaginary parts
    // nought) - an LU of a solenoid's 53,705 unknowns took 18.6 s.
    SparseComplexLdlt lu;
    // (One step of iterative refinement against the matrix: the pivots of a
    // saddle point scaled as it is lose a digit or two.)
    Eigen::SparseMatrix<double> matrix;
    double residual = 0;
    const auto solveWith = [&lu, &matrix, &residual](const Eigen::VectorXd& b) -> Eigen::VectorXd {
        Eigen::VectorXd x = lu.solve(b.cast<std::complex<double>>()).real();
        const Eigen::VectorXd r0 = b - matrix * x;
        x += lu.solve(r0.cast<std::complex<double>>()).real();
        const double bn = b.norm();
        residual = bn > 0 ? (b - matrix * x).norm() / bn : 0;
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
        assemble(q, a, J, a_magnets, M, r, gaugeScale);
        const qint64 assembled = clock.elapsed();
        lu.compute(M.cast<std::complex<double>>());
        matrix = M;
        if (it == 0)
            a_s.log << tr("%1: assembled in %2 s, %3 nonzeros; factorized in %4 s")
                           .arg(name)
                           .arg(assembled / 1000.0, 0, 'f', 2)
                           .arg(M.nonZeros())
                           .arg((clock.elapsed() - assembled) / 1000.0, 0, 'f', 2);
        if (lu.info() != Eigen::Success) {
            *error = tr("%1: its system could not be factorized (%2) - is A held somewhere (Magnetic Insulation, a Magnetic Potential)?")
                         .arg(name, QString::fromStdString(lu.lastErrorMessage()));
            return false;
        }
        if (it == 0)
            a_s.log << tr("%1: %2 unknowns (%3 of A on edge elements of order %4, %5 of the gauge's multiplier), %6")
                           .arg(name)
                           .arg(q.nA + q.nP)
                           .arg(q.nA)
                           .arg(es.order())
                           .arg(q.nP)
                           .arg(lu.name());
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

// ---------------------------------------------------------------- Globals

void MagneticProblem3::setGlobals()
{
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
            for (int c = 0; c < int(a_coils.size()); ++c) {
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
        s.setGlobal(p + QStringLiteral("Phi_") + c.name, c.flux, weber, tr("coil %1's flux linkage").arg(c.name));
        if (i0 != 0) s.setGlobal(p + QStringLiteral("L_") + c.name, c.flux / i0, henry, tr("coil %1's inductance: its flux linkage over its current").arg(c.name));
        static const char* const axes[3] = {"x", "y", "z"};
        for (int k = 0; k < 3; ++k)
            s.setGlobal(p + QStringLiteral("F") + QString::fromLatin1(axes[k]) + QLatin1Char('_') + c.name, c.force[k], force,
                        tr("the Lorentz force on coil %1, %2 component").arg(c.name, QString::fromLatin1(axes[k])));
    }
    const int nc = int(a_inductance.size());
    for (int i = 0; i < nc; ++i)
        for (int j = 0; j < nc; ++j) {
            const QString a = a_coils[std::size_t(i)].name, b = a_coils[std::size_t(j)].name;
            const double l = a_inductance[std::size_t(i)][std::size_t(j)];
            s.setGlobal(p + QStringLiteral("L_") + a + QLatin1Char('_') + b, l, henry, tr("inductance matrix, %1 %2").arg(a, b));
            if (a.size() == 1 && b.size() == 1) s.setGlobal(p + QStringLiteral("L") + a + b, l, henry, tr("inductance matrix, %1 %2").arg(a, b));
        }
}

} // namespace

std::unique_ptr<Problem3> makeMagneticProblem3(Solution3& solution, int field)
{
    return std::make_unique<MagneticProblem3>(solution, field);
}

bool magneticStateAt(PointEvaluator3& ev, int f, MagneticState& out)
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
    return true;
}

} // namespace qucs_s::fem::detail3
