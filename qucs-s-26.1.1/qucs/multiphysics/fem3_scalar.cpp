/*
 * fem3_scalar.cpp - the 3D scalar physics: electrostatics, electric currents
 *                   and heat transfer, each -∇·(c ∇u) = f on a swept mesh's
 *                   prisms (c a tensor where a crystal's is), their conditions
 *                   on the geometry's faces, terminals and their matrices; one
 *                   that reads its own field (k(T), radiation) solved by
 *                   Picard's iterations
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_problem.h"

#include "fem_problem.h"
#include "fem_tensor.h"

#include <Eigen/Dense>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <limits>
#include <thread>

namespace qucs_s::fem::detail3 {

namespace {

constexpr double Eps0 = 8.8541878188e-12;
const double NaN = std::numeric_limits<double>::quiet_NaN();

/// What a face has: a condition of its own (the last feature's).
struct Condition {
    enum Kind { None, Dirichlet, Group };
    Kind kind = None;
    int order = -1;        // its feature's place among the physics'
    Expression value;      // Dirichlet's
    int terminal = -1;
};

/// A flux a feature adds on a face: g in, and (a film's) q u out.
struct Natural {
    Expression g;
    Expression q;          // invalid: none
    bool made = true;      // g is heat (charge) made there, not exchanged (h Text)
};

struct TerminalDef {
    QString name;
    bool floating = false; // a group of one potential: a charge (current) fed
    Expression value;      // V0, or the charge (current) fed
};

/// A system's equations, each degree of freedom's.
struct Equations {
    std::vector<int> eq;        // by dof: its equation; -1 none (not solved there); -3 fixed
    std::vector<int> fixed;     // by dof: its index among the fixed, -1
    std::vector<int> fixedDof;  // by fixed index: its dof
    std::vector<int> fixedFace; // by fixed index: the face whose value it takes (-1: a sweep's)
    std::vector<int> terminal;  // by dof: its terminal (a fixed or group dof's), -1
    std::vector<int> groupEq;   // by terminal: its equation (floating), -1
    int neq = 0;
};

/// A system assembled: the free equations', and the fixed dofs' rows for
/// their reactions.
struct Assembled {
    Eigen::SparseMatrix<double> A;    // neq x neq
    Eigen::SparseMatrix<double> Kc;   // neq x fixed
    Eigen::SparseMatrix<double> R;    // fixed x neq
    Eigen::SparseMatrix<double> Rd;   // fixed x fixed
    Eigen::VectorXd F, Fd;
    Eigen::SparseMatrix<double> J;    // Newton's: the rest of its matrix (neq x neq), of the derivatives
    double made = 0;                  // what the sources make
    bool robin = false;
};

/// A tensor's texts in a 3D model's axes from the 2D convention's (x across,
/// y up - the wafer's normal -, z along): a 3D model's z is up, its y along,
/// x the same - turned 90° about x (y -> z, z -> -y): T3 = M T2 Mᵀ.
TensorText inThreeD(const TensorText& t)
{
    static const int from[3] = {0, 2, 1};       // the 2D axis each 3D one is
    static const double sign[3] = {1, -1, 1};   // and its sign
    TensorText out = t;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const QString& c = t.c[from[i]][from[j]];
            const double s = sign[i] * sign[j];
            out.c[i][j] = (s > 0 || c == QLatin1String("0")) ? c : QStringLiteral("-(%1)").arg(c);
        }
    return out;
}

/// The triplets of several lists in turn, as their concatenation: a
/// matrix made of parts' triplets, without copying them into one.
class ChainedTriplets
{
public:
    using T = Eigen::Triplet<double>;
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T*;
    using reference = const T&;
    ChainedTriplets(const std::vector<const std::vector<T>*>* lists, std::size_t list) : a_lists(lists), a_list(list) { skip(); }
    reference operator*() const { return (*(*a_lists)[a_list])[a_at]; }
    pointer operator->() const { return &**this; }
    ChainedTriplets& operator++()
    {
        ++a_at;
        skip();
        return *this;
    }
    bool operator==(const ChainedTriplets& o) const { return a_list == o.a_list && a_at == o.a_at; }
    bool operator!=(const ChainedTriplets& o) const { return !(*this == o); }

private:
    // (Past a list's end, or an empty one: the next list's first.)
    void skip()
    {
        while (a_list < a_lists->size() && a_at >= (*a_lists)[a_list]->size()) {
            ++a_list;
            a_at = 0;
        }
    }
    const std::vector<const std::vector<T>*>* a_lists;
    std::size_t a_list = 0, a_at = 0;
};

std::atomic<int> g_assemblyParts{0};

class ScalarProblem3 : public Problem3
{
public:
    using Problem3::Problem3;
    bool setup(QString* error) override;
    void initialize() override;
    bool solve(const Options3& o, QString* error) override;
    void setGlobals() override;
    bool hasMass() const override { return a_heat; }

private:
    void buildEquations(bool sweep, Equations& q) const;
    /// Each fixed dof's value: its condition's, or (a sweep's terminal \a
    /// sweepTerminal, from 0) 1 on that terminal and 0 elsewhere.
    std::vector<double> fixedValues(const Equations& q, int sweepTerminal) const;
    /// The system, and (a step in time's, \a mass not 0) its capacity's term
    /// C (mass u - history): mass C in the matrix, C history in the vector.
    /// \a jacobian: Newton's matrix's rest too (out.J) - the coefficients',
    /// sources' and films' derivatives in the field, found numerically.
    void assemble(const Equations& q, bool withSources, Assembled& out, double mass = 0, const std::vector<double>* history = nullptr,
                  bool jacobian = false);
    double evalAt(const Expression& e, const Vec3& x) const;
    /// Each terminal's reaction: what flows into the domain from its fixed
    /// dofs (charge, current).
    std::vector<double> reactions(const Equations& q, const Assembled& a, const Eigen::VectorXd& x, const Eigen::VectorXd& ud) const;
    bool sweepTerminals(const Options3& o, QString* error);

    bool a_es = false, a_ec = false, a_heat = false;
    double a_scale = 1;
    std::vector<Condition> a_conditions;          // by face
    std::vector<std::vector<Natural>> a_natural;  // by face
    std::vector<TerminalDef> a_terminals;
    std::vector<Expression> a_init;               // by domain
    std::vector<double> a_volume;                 // by domain (m³)
    bool a_self = false;
    bool a_converged = true;
    double a_made = 0;
    bool a_assembled = false;   // a_made of a system assembled (not at a step in time's start)
    double a_sink = NaN;
};

// ------------------------------------------------------------------ Setup

bool ScalarProblem3::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field3& f = field();
    const Model& model = a_s.model();
    const Topology3& topo = a_s.topology();
    const Mesh3& mesh = a_s.mesh();
    const QString type = a_node->type;
    a_es = type == QLatin1String("electrostatics");
    a_ec = type == QLatin1String("currents");
    a_heat = type == QLatin1String("heat");
    a_scale = a_es ? Eps0 : 1.0;
    const std::size_t D = topo.domains.size(), F = topo.faces.size();
    QString why;
    const QVector<int> domains = resolveSelection3(topo, model, Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(D, 0);
    for (int d : domains)
        if (d >= 0 && d < int(D)) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    const std::vector<const Node*> material = materials();
    const QString modelFeature = a_es ? QStringLiteral("electrostatics/charge") : a_ec ? QStringLiteral("currents/conservation") : QStringLiteral("heat/solid");
    const QString property = a_es ? QStringLiteral("epsilonr") : a_ec ? QStringLiteral("sigma") : QStringLiteral("k");
    // (A user's tensor's keys: epsxx..., sigmaxx..., kxx...)
    const QString tensorPrefix = a_es ? QStringLiteral("eps") : property;
    const QString propertyName = a_es ? tr("relative permittivity") : a_ec ? tr("electrical conductivity") : tr("thermal conductivity");
    std::vector<const Node*> modelOf(D, nullptr);
    Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    coef.c.assign(D, {});
    coef.isotropic.assign(D, 1);
    coef.sources.assign(D, {});
    coef.sourceScale.assign(D, 1.0);
    coef.capacity.assign(D, Expression());
    a_conditions.assign(F, Condition());
    a_natural.assign(F, {});
    a_terminals.clear();
    a_init.assign(D, Expression());
    // Each domain's volume (m³), for a total power spread over some.
    a_volume.assign(D, 0);
    {
        const Space3& sp = *f.space;
        const PrismRule& rule = prismRule(2, 1);
        for (int e = 0; e < sp.elements(); ++e)
            for (const auto& q : rule.points)
                a_volume[std::size_t(mesh.domain[std::size_t(e)])] += q[3] * std::abs(sp.map(e, q[0], q[1], q[2]).det);
    }
    int order = 0;
    for (const Node& feature : a_node->children) {
        ++order;
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k) continue;
        const QVector<int> picked = resolveSelection3(topo, model, k->selection, feature.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(feature.name(), why));
        if (k->selection == Level::Domain) {
            if (feature.type == modelFeature) {
                for (int d : picked) modelOf[std::size_t(d)] = &feature;
                continue;
            }
            if (feature.type == QLatin1String("heat/init")) {
                const Expression e = compile(feature.text(QStringLiteral("T0")), feature, QStringLiteral("T0"), false);
                for (int d : picked) a_init[std::size_t(d)] = e;
                continue;
            }
            // A source.
            Expression e;
            double scale = 1;
            if (feature.type == QLatin1String("heat/source") && feature.text(QStringLiteral("kind")) == QLatin1String("power")) {
                const std::optional<double> p = evaluateConstant(feature.text(QStringLiteral("P0")), a_s.parameters().scope, Dim::power(), &why);
                if (!p) {
                    problem(tr("%1: %2").arg(feature.name(), why));
                    continue;
                }
                double total = 0;
                for (int d : picked)
                    if (f.domains[std::size_t(d)]) total += a_volume[std::size_t(d)];
                if (total <= 0) continue;
                e = Expression::compile(QStringLiteral("1"), a_s.pointScope());
                scale = *p / total;
            } else {
                const QString key = a_es ? QStringLiteral("rho") : a_ec ? QStringLiteral("Qj") : QStringLiteral("Q0");
                e = compile(feature.text(key), feature, key);
            }
            if (!e.isValid()) continue;
            for (int d : picked) {
                if (!f.domains[std::size_t(d)]) continue;
                coef.sources[std::size_t(d)].push_back(e);
                coef.sourceScale[std::size_t(d)] = scale;
            }
            continue;
        }
        if (k->selection != Level::Boundary) continue;
        // Face features: those with a condition of their own (the last wins),
        // and fluxes (they add).
        const QString t = feature.type;
        const bool exclusive = t.endsWith(QLatin1String("/zerocharge")) || t.endsWith(QLatin1String("/insulation"))
                               || t.endsWith(QLatin1String("/ground")) || t.endsWith(QLatin1String("/potential"))
                               || t.endsWith(QLatin1String("/terminal")) || t.endsWith(QLatin1String("/floating"))
                               || t == QLatin1String("heat/temperature");
        if (exclusive) {
            Condition c;
            c.order = order;
            if (t.endsWith(QLatin1String("/ground"))) {
                c.kind = Condition::Dirichlet;
                c.value = Expression::compile(QStringLiteral("0"), a_s.pointScope());
            } else if (t.endsWith(QLatin1String("/potential"))) {
                c.kind = Condition::Dirichlet;
                c.value = compile(feature.text(QStringLiteral("V0")), feature, QStringLiteral("V0"), false);
            } else if (t == QLatin1String("heat/temperature")) {
                c.kind = Condition::Dirichlet;
                c.value = compile(feature.text(QStringLiteral("T0")), feature, QStringLiteral("T0"), false);
                if (c.value.isConstant()) a_sink = std::isnan(a_sink) ? c.value.constant() : std::min(a_sink, c.value.constant());
            } else if (t.endsWith(QLatin1String("/terminal")) || t.endsWith(QLatin1String("/floating"))) {
                TerminalDef term;
                term.name = t.endsWith(QLatin1String("/terminal")) ? feature.text(QStringLiteral("name")) : feature.name();
                term.floating = t.endsWith(QLatin1String("/floating"))
                                || (t == QLatin1String("currents/terminal") && feature.text(QStringLiteral("drive")) == QLatin1String("current"));
                const QString key = !term.floating ? QStringLiteral("V0") : a_es ? QStringLiteral("Q0") : QStringLiteral("I0");
                term.value = compile(feature.text(key), feature, key, false);
                // (One value over its faces: a constant, or of t alone - a
                // step in time's terminal driven.)
                if (term.value.isValid())
                    for (int slot : term.value.variables())
                        if (a_s.definitions()[std::size_t(slot)].kind != Solution3::VariableDef::Time) {
                            problem(tr("%1 of %2 must be a constant, or of t alone").arg(key, feature.name()));
                            break;
                        }
                c.terminal = int(a_terminals.size());
                if (term.floating) {
                    c.kind = Condition::Group;
                } else {
                    c.kind = Condition::Dirichlet;
                    c.value = term.value;
                }
                a_terminals.push_back(term);
            }
            for (int b : picked)
                if (b >= 0 && b < int(F)) a_conditions[std::size_t(b)] = c;
            continue;
        }
        Natural n;
        if (t == QLatin1String("electrostatics/surfacecharge")) n.g = compile(feature.text(QStringLiteral("rhos")), feature, QStringLiteral("rhos"));
        else if (t == QLatin1String("currents/normalcurrent")) n.g = compile(feature.text(QStringLiteral("Jn")), feature, QStringLiteral("Jn"));
        else if (t == QLatin1String("heat/linesource")) n.g = compile(feature.text(QStringLiteral("Qb")), feature, QStringLiteral("Qb"));
        else if (t == QLatin1String("heat/flux")) {
            if (feature.text(QStringLiteral("kind")) == QLatin1String("general")) {
                n.g = compile(feature.text(QStringLiteral("q0")), feature, QStringLiteral("q0"));
            } else {
                const Expression h = compile(feature.text(QStringLiteral("h")), feature, QStringLiteral("h"));
                const Expression text = compile(feature.text(QStringLiteral("Text")), feature, QStringLiteral("Text"));
                if (!h.isValid() || !text.isValid()) continue;
                n.q = h;
                n.made = false;
                n.g = Expression::compile(QStringLiteral("(") + feature.text(QStringLiteral("h")) + QStringLiteral(")*(")
                                              + feature.text(QStringLiteral("Text")) + QLatin1Char(')'),
                                          a_s.pointScope());
                if (text.isConstant()) a_sink = std::isnan(a_sink) ? text.constant() : std::min(a_sink, text.constant());
            }
        } else if (t == QLatin1String("heat/radiation")) {
            // εσ(Tamb⁴ - T⁴) = h_r (Tamb - T), h_r = εσ(Tamb² + T²)(Tamb + T): a
            // film of h_r, T's own (Picard's iterations).
            const Expression eps = compile(feature.text(QStringLiteral("epsilon")), feature, QStringLiteral("epsilon"));
            const Expression amb = compile(feature.text(QStringLiteral("Tamb")), feature, QStringLiteral("Tamb"));
            if (!eps.isValid() || !amb.isValid()) continue;
            const QString T = f.tag + QStringLiteral(".T");
            const QString e = QStringLiteral("(") + feature.text(QStringLiteral("epsilon")) + QLatin1Char(')');
            const QString a = QStringLiteral("(") + feature.text(QStringLiteral("Tamb")) + QLatin1Char(')');
            const QString hr = QStringLiteral("%1*sigma_const*(%2^2+%3^2)*(%2+%3)").arg(e, a, T);
            n.q = Expression::compile(hr, a_s.pointScope());
            n.g = Expression::compile(QStringLiteral("(%1)*%2").arg(hr, a), a_s.pointScope());
            if (!n.q.isValid() || !n.g.isValid()) {
                problem(tr("%1: %2").arg(feature.name(), n.q.isValid() ? n.g.error() : n.q.error()));
                continue;
            }
            a_reads.insert(a_fi);
            n.made = false;
            if (amb.isConstant()) a_sink = std::isnan(a_sink) ? amb.constant() : std::min(a_sink, amb.constant());
        } else {
            continue;
        }
        if (!n.g.isValid() && !n.q.isValid()) continue;
        for (int b : picked)
            if (b >= 0 && b < int(F)) a_natural[std::size_t(b)].push_back(n);
    }
    // Each domain's coefficient: its feature's own (isotropic), or its
    // material's - one value, or a crystal's tensor turned by its
    // orientation into the model's axes.
    const Scope& params = a_s.parameters().scope;
    for (int d = 0; d < int(D); ++d) {
        if (!f.domains[std::size_t(d)]) continue;
        const Node* feat = modelOf[std::size_t(d)];
        if (!feat) {
            problem(tr("domain %1 has no %2 feature").arg(d + 1).arg(modelFeature.section(QLatin1Char('/'), 1)));
            continue;
        }
        const bool user = feat->text(QStringLiteral("source")) == QLatin1String("user");
        const Node* m = material[std::size_t(d)];
        const QString object = topo.objectNames.value(topo.domains[std::size_t(d)].owner);
        TensorText t;
        const Node* where = feat;
        QString keys[3][3];
        for (auto& row : keys)
            for (QString& k : row) k = property;
        if (user) {
            // Its own value, or a tensor in the model's axes: xx, yy, zz and
            // (symmetric) xy, xz, yz - each read by its own key.
            const QString aniso = feat->text(QStringLiteral("anisotropy"));
            t.items = 1;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) t.c[i][j] = i == j ? feat->text(property) : QStringLiteral("0");
            if (aniso == QLatin1String("diagonal") || aniso == QLatin1String("symmetric")) {
                static const char axes[] = "xyz";
                const bool full = aniso == QLatin1String("symmetric");
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) {
                        if (i != j && !full) continue;
                        const QString key = tensorPrefix + QLatin1Char(axes[std::min(i, j)]) + QLatin1Char(axes[std::max(i, j)]);
                        keys[i][j] = key;
                        t.c[i][j] = feat->text(key);
                    }
                t.items = full ? 6 : 3;
            }
        } else if (!m) {
            problem(tr("domain %1 (%2) has no material: its %3 is needed").arg(d + 1).arg(object, propertyName));
            continue;
        } else {
            where = m;
            const QString text = detail::Problem::materialText(*m, property, &params);
            if (text.trimmed().isEmpty()) {
                problem(tr("domain %1 (%2): its material, %3, has no %4").arg(d + 1).arg(object, m->name(), propertyName));
                continue;
            }
            const std::optional<TensorText> crystal = tensorText(text, &why);
            if (!crystal) {
                problem(tr("%1's %2: %3").arg(m->name(), propertyName, why));
                continue;
            }
            t = *crystal;
            if (crystal->anisotropic()) {
                const std::optional<Rotation> r = materialRotation(*m, params, &why);
                if (!r) {
                    problem(why);
                    continue;
                }
                t = inThreeD(rotated(*crystal, *r));
            }
        }
        std::array<Expression, 9>& c = coef.c[std::size_t(d)];
        bool isotropic = true;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                const QString& text = t.c[i][j];
                if (text.trimmed().isEmpty()) {
                    problem(tr("domain %1 (%2): its %3 is not given").arg(d + 1).arg(object, propertyName));
                    continue;
                }
                c[std::size_t(3 * i + j)] = i != j && text == QLatin1String("0") ? Expression::compile(QStringLiteral("0"), a_s.pointScope())
                                                                                  : compile(text, *where, keys[i][j]);
                if (i != j && text != QLatin1String("0")) isotropic = false;
                if (i == j && text != t.c[0][0]) isotropic = false;
            }
        coef.isotropic[std::size_t(d)] = isotropic ? 1 : 0;
        for (int i = 0; i < 3; ++i) {
            const Expression& e = c[std::size_t(4 * i)];
            if (e.isValid() && e.isConstant() && !(e.constant() > 0) && !(a_es && e.constant() != 0))
                problem(tr("domain %1: its %2 must be more than 0").arg(d + 1).arg(propertyName));
        }
        if (a_heat) {
            // ρ Cp, for a step in time (said only then: at rest neither is
            // needed).
            const Node* r = user ? feat : m;
            QString rho, cp;
            if (r) {
                rho = user ? feat->text(QStringLiteral("rho")) : detail::Problem::materialText(*m, QStringLiteral("rho"), &params);
                cp = user ? feat->text(QStringLiteral("Cp")) : detail::Problem::materialText(*m, QStringLiteral("Cp"), &params);
            }
            if (!rho.trimmed().isEmpty() && !cp.trimmed().isEmpty()) {
                const Expression er = compile(rho, *r, QStringLiteral("rho"));
                const Expression ec = compile(cp, *r, QStringLiteral("Cp"));
                if (er.isValid() && ec.isValid())
                    coef.capacity[std::size_t(d)] = Expression::compile(QStringLiteral("(") + rho + QStringLiteral(")*(") + cp + QLatin1Char(')'),
                                                                       a_s.pointScope());
            }
        }
    }
    a_self = a_reads.contains(a_fi);
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    return true;
}

void ScalarProblem3::initialize()
{
    Field3& f = field();
    const Space3& sp = *f.space;
    const Mesh3& mesh = a_s.mesh();
    f.values.assign(std::size_t(sp.size()), f.initial);
    if (!a_heat) return;
    // Each domain's initial temperature, at its dofs (heat/init).
    for (int e = 0; e < sp.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)] || !a_init[std::size_t(d)].isValid()) continue;
        const int* dofs = sp.dofs(e);
        for (int i = 0; i < sp.perElement(); ++i) {
            const double v = evalAt(a_init[std::size_t(d)], sp.point(dofs[i]));
            if (std::isfinite(v)) f.values[std::size_t(dofs[i])] = v;
        }
    }
}

double ScalarProblem3::evalAt(const Expression& e, const Vec3& x) const
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

void ScalarProblem3::buildEquations(bool sweep, Equations& q) const
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const Space3& sp = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const std::size_t n = std::size_t(sp.size());
    q = Equations();
    q.eq.assign(n, -1);
    q.fixed.assign(n, -1);
    q.terminal.assign(n, -1);
    q.groupEq.assign(a_terminals.size(), -1);
    // The dofs of its domains' prisms: free (-2) until a condition holds them.
    for (int e = 0; e < sp.elements(); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        const int* dofs = sp.dofs(e);
        for (int i = 0; i < sp.perElement(); ++i) q.eq[std::size_t(dofs[i])] = -2;
    }
    // The faces with a condition, in their features' order: a later one's
    // holds a dof two share; a fixed value over a group's.
    std::vector<std::pair<int, int>> held;   // (order, mesh face)
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
        const Condition& c = a_conditions[std::size_t(mesh.faces[std::size_t(fi)].face)];
        if (c.kind != Condition::None) held.emplace_back(c.order, fi);
    }
    std::stable_sort(held.begin(), held.end());
    std::vector<int> fixedFaceOf(n, -1);
    std::vector<char> group(n, 0);
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& [ord, fi] : held) {
            (void)ord;
            const int face = mesh.faces[std::size_t(fi)].face;
            const Condition& c = a_conditions[std::size_t(face)];
            // (A sweep holds every terminal, floating ones too.)
            const bool fixed = c.kind == Condition::Dirichlet || sweep;
            if ((pass == 0) == fixed) continue;   // groups first, then the fixed over them
            for (int dof : sp.faceDofs(fi)) {
                if (dof < 0 || q.eq[std::size_t(dof)] == -1) continue;
                if (fixed) {
                    fixedFaceOf[std::size_t(dof)] = face;
                    group[std::size_t(dof)] = 0;
                    q.eq[std::size_t(dof)] = -3;
                } else {
                    group[std::size_t(dof)] = 1;
                }
                q.terminal[std::size_t(dof)] = c.terminal;
            }
        }
    // The equations: each floating terminal's one, then each free dof's.
    for (std::size_t dof = 0; dof < n; ++dof) {
        if (q.eq[dof] == -3) {
            q.fixed[dof] = int(q.fixedDof.size());
            q.fixedDof.push_back(int(dof));
            q.fixedFace.push_back(sweep && q.terminal[dof] >= 0 ? -1 : fixedFaceOf[dof]);
            continue;
        }
        if (q.eq[dof] != -2) continue;
        if (group[dof] && q.terminal[dof] >= 0) {
            int& g = q.groupEq[std::size_t(q.terminal[dof])];
            if (g < 0) g = q.neq++;
            q.eq[dof] = g;
        } else {
            q.eq[dof] = q.neq++;
            q.terminal[dof] = -1;
        }
    }
}

std::vector<double> ScalarProblem3::fixedValues(const Equations& q, int sweepTerminal) const
{
    const Space3& sp = *a_s.fields()[std::size_t(a_fi)].space;
    std::vector<double> out(q.fixedDof.size(), 0);
    for (std::size_t k = 0; k < q.fixedDof.size(); ++k) {
        const int dof = q.fixedDof[k];
        // (A sweep's: its terminal at 1 V, every other fixed value 0.)
        if (sweepTerminal >= 0) {
            out[k] = q.terminal[std::size_t(dof)] == sweepTerminal ? 1.0 : 0.0;
            continue;
        }
        const int face = q.fixedFace[k];
        out[k] = face >= 0 ? evalAt(a_conditions[std::size_t(face)].value, sp.point(dof)) : 0.0;
    }
    return out;
}

// --------------------------------------------------------------- Assembly

void ScalarProblem3::assemble(const Equations& q, bool withSources, Assembled& out, double mass, const std::vector<double>* history,
                              bool jacobian)
{
    const Field3& f = a_s.fields()[std::size_t(a_fi)];
    const Space3& sp = *f.space;
    const Mesh3& mesh = a_s.mesh();
    const Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const int nfix = int(q.fixedDof.size());
    using T = Eigen::Triplet<double>;
    std::vector<T> ta, tc, tr, td, tj;
    out.F = Eigen::VectorXd::Zero(q.neq);
    out.Fd = Eigen::VectorXd::Zero(nfix);
    out.made = 0;
    out.robin = false;
    const int per = sp.perElement();
    // (A face's matrix and vector into the system: its free rows' and its
    // fixed rows'.)
    auto scatter = [&q, &out, &ta, &tc, &tr, &td](const std::vector<int>& dofs, const std::vector<double>& K, const std::vector<double>& Fe) {
        const int m = int(dofs.size());
        for (int i = 0; i < m; ++i) {
            const int di = dofs[std::size_t(i)];
            if (di < 0) continue;
            const int ei = q.eq[std::size_t(di)], fi = q.fixed[std::size_t(di)];
            if (ei >= 0) out.F(ei) += Fe[std::size_t(i)];
            else if (fi >= 0) out.Fd(fi) += Fe[std::size_t(i)];
            else continue;
            for (int j = 0; j < m; ++j) {
                const int dj = dofs[std::size_t(j)];
                if (dj < 0) continue;
                const double v = K[std::size_t(i * m + j)];
                if (v == 0) continue;
                const int ej = q.eq[std::size_t(dj)], fj = q.fixed[std::size_t(dj)];
                if (ei >= 0) {
                    if (ej >= 0) ta.emplace_back(ei, ej, v);
                    else if (fj >= 0) tc.emplace_back(ei, fj, v);
                } else {
                    if (ej >= 0) tr.emplace_back(fi, ej, v);
                    else if (fj >= 0) td.emplace_back(fi, fj, v);
                }
            }
        }
    };
    // The elements in parts on threads, each its own evaluator, triplets and
    // record of what it adds to F, Fd and the power made - joined in the
    // elements' order, so the matrix and vectors are one thread's, bit for
    // bit, whatever the threads (the triplets read in turn, not copied: the
    // assembly's memory as one thread's).
    struct Part {
        std::vector<T> ta, tc, tr, td, tj;
        std::vector<std::pair<int, double>> f, fd;
        std::vector<double> made;
    };
    // (Newton's: where the field reads itself - its coefficient, its source.)
    const bool newton = jacobian && a_self;
    const auto scatterInto = [&q](Part& p, const std::vector<int>& dofs, const std::vector<double>& K, const std::vector<double>& Fe) {
        const int m = int(dofs.size());
        for (int i = 0; i < m; ++i) {
            const int di = dofs[std::size_t(i)];
            if (di < 0) continue;
            const int ei = q.eq[std::size_t(di)], fi = q.fixed[std::size_t(di)];
            if (ei >= 0) p.f.emplace_back(ei, Fe[std::size_t(i)]);
            else if (fi >= 0) p.fd.emplace_back(fi, Fe[std::size_t(i)]);
            else continue;
            for (int j = 0; j < m; ++j) {
                const int dj = dofs[std::size_t(j)];
                if (dj < 0) continue;
                const double v = K[std::size_t(i * m + j)];
                if (v == 0) continue;
                const int ej = q.eq[std::size_t(dj)], fj = q.fixed[std::size_t(dj)];
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
    // (Its captures named: a long lambda's [&] crashed CI's GCC, d7ae967.)
    const auto assemblePart = [this, &parts, &sp, &mesh, &f, &coef, &rule, &scatterInto, &q, E, partCount, per, mass, withSources, history,
                               newton](int k) {
        Part& part = parts[std::size_t(k)];
        const int from = int(qint64(E) * k / partCount), to = int(qint64(E) * (k + 1) / partCount);
        part.ta.reserve(std::size_t(to - from) * std::size_t(per) * std::size_t(per));
        PointEvaluator3 ev(a_s);
        std::vector<int> dofs(static_cast<std::size_t>(per));
        std::vector<double> K(static_cast<std::size_t>(per * per)), Fe(static_cast<std::size_t>(per));
        std::vector<double> Me(mass != 0 ? static_cast<std::size_t>(per * per) : 0);
        std::vector<double> grads(static_cast<std::size_t>(per) * 3);
        std::vector<double> Je(newton ? static_cast<std::size_t>(per * per) : 0);
        for (int e = from; e < to; ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            const std::array<Expression, 9>& c = coef.c[std::size_t(d)];
            bool constant = true;
            double C[9];
            for (int k = 0; k < 9; ++k) {
                constant = constant && c[std::size_t(k)].isConstant();
                C[k] = c[std::size_t(k)].isConstant() ? c[std::size_t(k)].constant() : 0;
            }
            const std::vector<Expression>& sources = coef.sources[std::size_t(d)];
            const bool sourced = withSources && !sources.empty();
            // (A step in time's capacity, ρ Cp.)
            const bool massed = mass != 0 && coef.capacity[std::size_t(d)].isValid();
            const Expression& cap = coef.capacity[std::size_t(d)];
            double capv = massed && cap.isConstant() ? cap.constant() : 0;
            std::fill(K.begin(), K.end(), 0.0);
            std::fill(Fe.begin(), Fe.end(), 0.0);
            std::fill(Me.begin(), Me.end(), 0.0);
            std::fill(Je.begin(), Je.end(), 0.0);
            // (Its coefficient or source of the field: their derivatives.)
            const bool derived = newton && (!constant || sourced);
            const int* ed = sp.dofs(e);
            for (int i = 0; i < per; ++i) dofs[std::size_t(i)] = ed[i];
            for (const auto& qp : rule.points) {
                const Space3::Map m = sp.map(e, qp[0], qp[1], qp[2]);
                const double w = qp[3] * std::abs(m.det);
                PrismShape s;
                prismShapeAt(sp.order(), qp[0], qp[1], qp[2], s);
                for (int i = 0; i < per; ++i) Space3::gradient(m, s.dxi[i], s.deta[i], s.dzeta[i], &grads[std::size_t(3 * i)]);
                if (!constant || sourced || (massed && !cap.isConstant())) ev.at(e, qp[0], qp[1], qp[2], m.x);
                // Newton's: the coefficient's and the source's derivatives in
                // the field here, by a perturbation of its value - ∂(c ∇u)/∂u_j
                // = c ∇φ_j + c' φ_j ∇u, and -Q' φ_j.
                if (derived) {
                    double u = 0, g[3] = {0, 0, 0};
                    ev.field(a_fi, u, g);
                    double c0[9], q0 = 0;
                    for (int k = 0; k < 9; ++k) c0[k] = c[std::size_t(k)].isConstant() ? c[std::size_t(k)].constant() : ev.eval(c[std::size_t(k)]);
                    if (sourced)
                        for (const Expression& src : sources) q0 += ev.eval(src);
                    const double du = 1e-7 * (std::abs(u) + 1);
                    ev.perturb(a_fi, du);
                    double dc[9], dq = 0;
                    for (int k = 0; k < 9; ++k) dc[k] = c[std::size_t(k)].isConstant() ? 0 : (ev.eval(c[std::size_t(k)]) - c0[k]) / du;
                    if (sourced) {
                        double q1 = 0;
                        for (const Expression& src : sources) q1 += ev.eval(src);
                        dq = (q1 - q0) / du * coef.sourceScale[std::size_t(d)];
                    }
                    ev.perturb(a_fi, 0);
                    const double dcg[3] = {dc[0] * g[0] + dc[1] * g[1] + dc[2] * g[2], dc[3] * g[0] + dc[4] * g[1] + dc[5] * g[2],
                                           dc[6] * g[0] + dc[7] * g[1] + dc[8] * g[2]};
                    for (int i = 0; i < per; ++i) {
                        const double* gi = &grads[std::size_t(3 * i)];
                        const double flux = gi[0] * dcg[0] + gi[1] * dcg[1] + gi[2] * dcg[2];
                        for (int j = 0; j < per; ++j)
                            Je[std::size_t(i * per + j)] += w * (a_scale * flux * s.n[j] - (std::isfinite(dq) ? dq : 0) * s.n[i] * s.n[j]);
                    }
                }
                if (!constant)
                    for (int k = 0; k < 9; ++k) C[k] = c[std::size_t(k)].isConstant() ? c[std::size_t(k)].constant() : ev.eval(c[std::size_t(k)]);
                if (massed && !cap.isConstant()) capv = ev.eval(cap);
                if (sp.order() == 1) {
                    // A linear prism lumped: its in-plane term's factor along z,
                    // ∫ L_k L_l dz, and its vertical term's in the plane, ∫ T_a
                    // T_b dA, each as its row's sum. A thin layer's prisms - far
                    // wider than tall - otherwise couple neighbours in the plane
                    // the wrong way (positive), and a field undershoots where it
                    // falls steeply in z: the chip heater's oxide top, far from its
                    // cold back, 0.72 K under it. Linear fields and a field uniform
                    // in z stay exact; the tensor's cross terms (xz, yz) as they
                    // are.
                    Shape t;
                    shapeAt(1, qp[0], qp[1], t);
                    const double L[2] = {1 - qp[2], qp[2]}, dL[2] = {-m.invDz, m.invDz};
                    double gT[3][2];
                    for (int a = 0; a < 3; ++a) {
                        gT[a][0] = m.invT[0][0] * t.dxi[a] + m.invT[0][1] * t.deta[a];
                        gT[a][1] = m.invT[1][0] * t.dxi[a] + m.invT[1][1] * t.deta[a];
                    }
                    for (int i = 0; i < 6; ++i) {
                        const int a = i % 3, k = i / 3;
                        for (int j = 0; j < 6; ++j) {
                            const int b = j % 3, l = j / 3;
                            double v = 0;
                            if (k == l)
                                v += L[k] * (gT[a][0] * (C[0] * gT[b][0] + C[1] * gT[b][1]) + gT[a][1] * (C[3] * gT[b][0] + C[4] * gT[b][1]));
                            if (a == b) v += C[8] * t.n[a] * dL[k] * dL[l];
                            v += (gT[a][0] * C[2] + gT[a][1] * C[5]) * L[k] * t.n[b] * dL[l];
                            v += t.n[a] * dL[k] * (C[6] * gT[b][0] + C[7] * gT[b][1]) * L[l];
                            K[std::size_t(i * per + j)] += w * a_scale * v;
                            // The capacity's: consistent in the plane, as the
                            // triangles' in 2D, its factor along z lumped as the
                            // in-plane term's is - a tall prism's two levels
                            // otherwise coupled the wrong way at a short step.
                            if (massed && k == l) Me[std::size_t(i * per + j)] += w * capv * t.n[a] * t.n[b] * L[k];
                        }
                    }
                } else {
                    for (int j = 0; j < per; ++j) {
                        const double* gj = &grads[std::size_t(3 * j)];
                        const double cg[3] = {C[0] * gj[0] + C[1] * gj[1] + C[2] * gj[2], C[3] * gj[0] + C[4] * gj[1] + C[5] * gj[2],
                                              C[6] * gj[0] + C[7] * gj[1] + C[8] * gj[2]};
                        for (int i = 0; i < per; ++i) {
                            const double* gi = &grads[std::size_t(3 * i)];
                            K[std::size_t(i * per + j)] += w * a_scale * (gi[0] * cg[0] + gi[1] * cg[1] + gi[2] * cg[2]);
                            if (massed) Me[std::size_t(i * per + j)] += w * capv * s.n[i] * s.n[j];
                        }
                    }
                }
                if (sourced) {
                    double value = 0;
                    for (const Expression& src : sources) value += ev.eval(src);
                    value *= coef.sourceScale[std::size_t(d)];
                    if (!std::isfinite(value)) value = 0;
                    for (int i = 0; i < per; ++i) Fe[std::size_t(i)] += w * value * s.n[i];
                    part.made.push_back(w * value);
                }
            }
            if (massed)
                for (int i = 0; i < per; ++i)
                    for (int j = 0; j < per; ++j) {
                        const double v = Me[std::size_t(i * per + j)];
                        K[std::size_t(i * per + j)] += mass * v;
                        if (history) Fe[std::size_t(i)] += v * (*history)[std::size_t(dofs[std::size_t(j)])];
                    }
            scatterInto(part, dofs, K, Fe);
            if (derived)
                for (int i = 0; i < per; ++i) {
                    const int ei = dofs[std::size_t(i)] >= 0 ? q.eq[std::size_t(dofs[std::size_t(i)])] : -1;
                    if (ei < 0) continue;
                    for (int j = 0; j < per; ++j) {
                        const int ej = dofs[std::size_t(j)] >= 0 ? q.eq[std::size_t(dofs[std::size_t(j)])] : -1;
                        const double v = Je[std::size_t(i * per + j)];
                        if (ej >= 0 && v != 0) part.tj.emplace_back(ei, ej, v);
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
    for (Part& p : parts) {
        for (const auto& [i, v] : p.f) out.F(i) += v;
        for (const auto& [i, v] : p.fd) out.Fd(i) += v;
        for (double v : p.made) out.made += v;
        p.f = {};
        p.fd = {};
        p.made = {};
    }
    PointEvaluator3 ev(a_s);
    // The faces' fluxes and films.
    const int degree = sp.order() == 1 ? 2 : 4;
    for (int fi = 0; fi < int(mesh.faces.size()); ++fi) {
        const int face = mesh.faces[std::size_t(fi)].face;
        const std::vector<Natural>& list = a_natural[std::size_t(face)];
        if (list.empty()) continue;
        const std::vector<int> fd = sp.faceDofs(fi);
        bool on = !fd.empty();
        for (int dof : fd) on = on && dof >= 0 && q.eq[std::size_t(dof)] != -1;
        if (!on) continue;
        // (A field read on the face: on the side of a domain it is solved in.)
        const Topology3::Face& tf = a_s.topology().faces[std::size_t(face)];
        const int side = tf.back >= 0 && f.domains[std::size_t(tf.back)] ? tf.back : tf.front;
        const std::vector<Space3::FacePoint> points = sp.facePoints(fi, degree, side);
        const int m = int(fd.size());
        std::vector<double> Kf(static_cast<std::size_t>(m * m), 0.0), Ff(static_cast<std::size_t>(m), 0.0);
        std::vector<double> Jf(newton ? static_cast<std::size_t>(m * m) : 0, 0.0);
        for (const Natural& nat : list) {
            for (const Space3::FacePoint& fp : points) {
                if (fp.prism >= 0) ev.at(fp.prism, fp.xi, fp.eta, fp.zeta, fp.x);
                // Newton's: a film of the field's own (radiation's h, its g)
                // - the derivative of h u - g in u, h' u - g'.
                if (newton && fp.prism >= 0 && ((nat.q.isValid() && !nat.q.isConstant()) || (withSources && nat.g.isValid() && !nat.g.isConstant()))) {
                    double u = 0, g[3];
                    ev.field(a_fi, u, g);
                    const double h0 = nat.q.isValid() ? ev.eval(nat.q) : 0, g0 = withSources && nat.g.isValid() ? ev.eval(nat.g) : 0;
                    const double du = 1e-7 * (std::abs(u) + 1);
                    ev.perturb(a_fi, du);
                    const double h1 = nat.q.isValid() ? ev.eval(nat.q) : 0, g1 = withSources && nat.g.isValid() ? ev.eval(nat.g) : 0;
                    ev.perturb(a_fi, 0);
                    const double d = ((h1 - h0) * u - (g1 - g0)) / du;
                    if (std::isfinite(d))
                        for (int i = 0; i < m; ++i)
                            for (int j = 0; j < m; ++j) Jf[std::size_t(i * m + j)] += fp.weight * d * fp.n[i] * fp.n[j];
                }
                if (nat.q.isValid()) {
                    const double h = nat.q.isConstant() ? nat.q.constant() : ev.eval(nat.q);
                    if (std::isfinite(h) && h != 0) {
                        out.robin = true;
                        for (int i = 0; i < m; ++i)
                            for (int j = 0; j < m; ++j) Kf[std::size_t(i * m + j)] += fp.weight * h * fp.n[i] * fp.n[j];
                    }
                }
                if (withSources && nat.g.isValid()) {
                    const double g = nat.g.isConstant() ? nat.g.constant() : ev.eval(nat.g);
                    if (!std::isfinite(g)) continue;
                    for (int i = 0; i < m; ++i) Ff[std::size_t(i)] += fp.weight * g * fp.n[i];
                    if (nat.made) out.made += fp.weight * g;
                }
            }
        }
        scatter(fd, Kf, Ff);
        if (newton)
            for (int i = 0; i < m; ++i) {
                const int ei = fd[std::size_t(i)] >= 0 ? q.eq[std::size_t(fd[std::size_t(i)])] : -1;
                if (ei < 0) continue;
                for (int j = 0; j < m; ++j) {
                    const int ej = fd[std::size_t(j)] >= 0 ? q.eq[std::size_t(fd[std::size_t(j)])] : -1;
                    const double v = Jf[std::size_t(i * m + j)];
                    if (ej >= 0 && v != 0) tj.emplace_back(ei, ej, v);
                }
            }
    }
    const auto matrix = [&parts](Eigen::SparseMatrix<double>& into, std::vector<T> Part::*member, const std::vector<T>& faces) {
        std::vector<const std::vector<T>*> lists;
        for (const Part& p : parts) lists.push_back(&(p.*member));
        lists.push_back(&faces);
        into.setFromTriplets(ChainedTriplets(&lists, 0), ChainedTriplets(&lists, lists.size()));
    };
    out.A.resize(q.neq, q.neq);
    matrix(out.A, &Part::ta, ta);
    out.Kc.resize(q.neq, nfix);
    matrix(out.Kc, &Part::tc, tc);
    out.R.resize(nfix, q.neq);
    matrix(out.R, &Part::tr, tr);
    out.Rd.resize(nfix, nfix);
    matrix(out.Rd, &Part::td, td);
    if (newton) {
        out.J.resize(q.neq, q.neq);
        matrix(out.J, &Part::tj, tj);
    }
}

std::vector<double> ScalarProblem3::reactions(const Equations& q, const Assembled& a, const Eigen::VectorXd& x, const Eigen::VectorXd& ud) const
{
    std::vector<double> out(a_terminals.size(), 0);
    if (q.fixedDof.empty()) return out;
    const Eigen::VectorXd r = a.R * x + a.Rd * ud - a.Fd;
    for (std::size_t k = 0; k < q.fixedDof.size(); ++k) {
        const int t = q.terminal[std::size_t(q.fixedDof[k])];
        if (t >= 0) out[std::size_t(t)] += r(Eigen::Index(k));
    }
    return out;
}

// ------------------------------------------------------------------ Solve

bool ScalarProblem3::solve(const Options3& o, QString* error)
{
    Field3& f = field();
    const bool inTime = o.mass != 0 && a_heat;
    if (inTime) {
        const Solution3::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
        for (int d = 0; d < int(f.domains.size()); ++d)
            if (f.domains[std::size_t(d)] && !coef.capacity[std::size_t(d)].isValid()) {
                const Topology3& topo = a_s.topology();
                *error = tr("%1: domain %2 (%3) has no density and heat capacity (rho, Cp): a step in time needs them")
                             .arg(a_node->name())
                             .arg(d + 1)
                             .arg(topo.objectNames.value(topo.domains[std::size_t(d)].owner));
                return false;
            }
    }
    Equations q;
    buildEquations(false, q);
    const std::vector<double> fixedList = fixedValues(q, -1);
    const Eigen::VectorXd ud = Eigen::Map<const Eigen::VectorXd>(fixedList.data(), Eigen::Index(fixedList.size()));
    const QString name = a_node->name();
    const int rounds = a_self ? o.maxIterations : 1;
    // (A field that reads itself: by Newton's method, unless Picard's is
    // asked - its steps converge in a few where Picard's take many.)
    const bool newton = a_self && o.nonlinear != QLatin1String("picard");
    a_converged = !a_self;
    Eigen::VectorXd x = Eigen::VectorXd::Zero(q.neq);
    // (The field as it is: an iterative solve's guess.)
    for (std::size_t dof = 0; dof < f.values.size(); ++dof)
        if (q.eq[dof] >= 0) x(q.eq[dof]) = f.values[dof];
    for (int it = 0; it < rounds; ++it) {
        if (o.cancelled && o.cancelled()) {
            *error = tr("cancelled");
            return false;
        }
        Assembled a;
        assemble(q, true, a, inTime ? o.mass : 0.0, inTime ? o.history : nullptr, newton);
        if (it == 0 && q.fixedDof.empty() && !a.robin && !inTime) {
            *error = a_heat ? tr("%1 holds the temperature nowhere: a Temperature, a convective Heat Flux or Radiation on a face is "
                                 "needed")
                                  .arg(name)
                            : tr("%1 holds the potential nowhere: a Ground, an Electric Potential or a Terminal on a face is needed")
                                  .arg(name);
            return false;
        }
        Eigen::VectorXd b = a.F;
        if (ud.size() > 0) b -= a.Kc * ud;
        for (std::size_t t = 0; t < a_terminals.size(); ++t)
            if (a_terminals[t].floating && q.groupEq[t] >= 0) b(q.groupEq[t]) += evalAt(a_terminals[t].value, Vec3{});
        System3 sys;
        if (newton) {
            // Newton's step: (A + J) δ = b - A x, from the field as it is -
            // its matrix not symmetric.
            const Eigen::VectorXd r = b - a.A * x;
            const Eigen::SparseMatrix<double> jm = a.A + a.J;
            Eigen::VectorXd delta = Eigen::VectorXd::Zero(q.neq);
            if (!sys.factorize(jm, o.solver, 1, o.tolerance, name, &a_s.log, error, false)) return false;
            if (!sys.solve(r, delta, name, &a_s.log, error)) return false;
            x += delta;
        } else {
            if (!sys.factorize(a.A, o.solver, 1, o.tolerance, name, &a_s.log, error)) return false;
            if (!sys.solve(b, x, name, &a_s.log, error)) return false;
        }
        // The field: each dof's equation's value, or its fixed one.
        const std::vector<double> before = f.values;
        for (std::size_t dof = 0; dof < f.values.size(); ++dof) {
            if (q.eq[dof] >= 0) f.values[dof] = x(q.eq[dof]);
            else if (q.fixed[dof] >= 0) f.values[dof] = ud(q.fixed[dof]);
        }
        a_made = a.made;
        a_assembled = true;
        // Its terminals: their potentials and what flows from them.
        const std::vector<double> flow = reactions(q, a, x, ud);
        f.terminals.clear();
        for (std::size_t t = 0; t < a_terminals.size(); ++t) {
            Field3::Terminal term;
            term.name = a_terminals[t].name;
            term.floating = a_terminals[t].floating;
            if (term.floating) {
                term.potential = q.groupEq[t] >= 0 ? x(q.groupEq[t]) : NaN;
                term.charge = evalAt(a_terminals[t].value, Vec3{});
            } else {
                term.potential = evalAt(a_terminals[t].value, Vec3{});
                term.charge = flow[t];
            }
            f.terminals.push_back(term);
        }
        if (!a_self) break;
        double num = 0, den = 0;
        for (std::size_t k = 0; k < f.values.size(); ++k) {
            const double dv = f.values[k] - before[k];
            num += dv * dv;
            den += f.values[k] * f.values[k];
        }
        const double change = den > 0 ? std::sqrt(num / den) : std::sqrt(num);
        a_s.log << (newton ? tr("%1, Newton's step %2: relative change %3") : tr("%1, Picard's iteration %2: relative change %3"))
                       .arg(name)
                       .arg(it + 1)
                       .arg(change, 0, 'g', 3);
        if (change < o.tolerance) {
            a_converged = true;
            break;
        }
    }
    if (!a_converged) {
        *error = (newton ? tr("%1: Newton's method did not converge in %2 steps") : tr("%1: Picard's iterations did not converge in %2"))
                     .arg(name)
                     .arg(o.maxIterations);
        return false;
    }
    f.solved = true;
    f.matrix.clear();
    if ((a_es || a_ec) && o.sweep && !a_terminals.empty()) return sweepTerminals(o, error);
    return true;
}

bool ScalarProblem3::sweepTerminals(const Options3& o, QString* error)
{
    // Each terminal at 1 V in turn, every other held value at 0 and no
    // sources: one matrix, a right-hand side a terminal.
    Field3& f = field();
    Equations q;
    buildEquations(true, q);
    Assembled a;
    assemble(q, false, a);
    const int n = int(a_terminals.size());
    System3 sys;
    const QString name = tr("%1, terminal sweep").arg(a_node->name());
    if (!sys.factorize(a.A, o.solver, n, o.tolerance, name, &a_s.log, error)) return false;
    f.matrix.assign(std::size_t(n), std::vector<double>(std::size_t(n), 0));
    for (int j = 0; j < n; ++j) {
        if (o.cancelled && o.cancelled()) {
            *error = tr("cancelled");
            return false;
        }
        const std::vector<double> fixedList = fixedValues(q, j);
        const Eigen::VectorXd ud = Eigen::Map<const Eigen::VectorXd>(fixedList.data(), Eigen::Index(fixedList.size()));
        Eigen::VectorXd b = Eigen::VectorXd::Zero(q.neq);
        if (ud.size() > 0) b -= a.Kc * ud;
        Eigen::VectorXd x = Eigen::VectorXd::Zero(q.neq);
        QStringList quiet;
        if (!sys.solve(b, x, name, j == 0 ? &a_s.log : &quiet, error)) return false;
        const std::vector<double> flow = reactions(q, a, x, ud);
        for (int i = 0; i < n; ++i) f.matrix[std::size_t(i)][std::size_t(j)] = flow[std::size_t(i)];
    }
    return true;
}

// ---------------------------------------------------------------- Globals

QString matrixName(const QString& tag, const QString& letter, const QString& a, const QString& b)
{
    return tag + QLatin1Char('.') + letter + QLatin1Char('_') + a + QLatin1Char('_') + b;
}

void ScalarProblem3::setGlobals()
{
    Solution3& s = a_s;
    Field3& f = field();
    const QString p = f.tag + QLatin1Char('.');
    const Dim charge = a_es ? Dim::charge() : Dim::current();
    const QString qn = a_es ? QStringLiteral("Q") : QStringLiteral("I");
    for (const Field3::Terminal& t : f.terminals) {
        if (a_heat) break;
        s.setGlobal(p + QStringLiteral("V_") + t.name, t.potential, Dim::voltage(), tr("terminal %1's potential").arg(t.name));
        s.setGlobal(p + qn + QLatin1Char('_') + t.name, t.charge, charge,
                    a_es ? tr("terminal %1's charge").arg(t.name) : tr("the current out of terminal %1").arg(t.name));
    }
    const int n = int(f.matrix.size());
    if (n > 0) {
        const QString letter = a_es ? QStringLiteral("C") : QStringLiteral("G");
        const Dim dim = a_es ? Dim::capacitance() : Dim::conductance();
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                const QString a = f.terminals[std::size_t(i)].name, b = f.terminals[std::size_t(j)].name;
                const double v = f.matrix[std::size_t(i)][std::size_t(j)];
                const QString what = a_es ? tr("capacitance matrix, %1 %2").arg(a, b) : tr("conductance matrix, %1 %2").arg(a, b);
                s.setGlobal(matrixName(f.tag, letter, a, b), v, dim, what);
                if (a.size() == 1 && b.size() == 1) s.setGlobal(p + letter + a + b, v, dim, what);
            }
        if (a_ec) {
            s.setGlobal(p + QStringLiteral("R"), 1.0 / f.matrix[0][0], Dim::resistance(),
                        tr("resistance from terminal %1 to the others and the grounds").arg(f.terminals[0].name));
            Eigen::MatrixXd g(n, n);
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) g(i, j) = f.matrix[std::size_t(i)][std::size_t(j)];
            Eigen::FullPivLU<Eigen::MatrixXd> lu(g);
            if (lu.isInvertible()) {
                const Eigen::MatrixXd r = lu.inverse();
                for (int i = 0; i < n; ++i)
                    for (int j = 0; j < n; ++j) {
                        const QString a = f.terminals[std::size_t(i)].name, b = f.terminals[std::size_t(j)].name;
                        s.setGlobal(matrixName(f.tag, QStringLiteral("R"), a, b), r(i, j), Dim::resistance(), tr("resistance matrix, %1 %2").arg(a, b));
                        if (a.size() == 1 && b.size() == 1) s.setGlobal(p + QStringLiteral("R") + a + b, r(i, j), Dim::resistance());
                    }
            }
        }
    }
    // ∫ ∇u·c∇u over its domains: twice the electric energy (es), the power
    // (ec).
    const Space3& sp = *f.space;
    const Mesh3& mesh = s.mesh();
    const Solution3::Coefficients& coef = s.coefficients()[std::size_t(a_fi)];
    if (a_es || a_ec) {
        PointEvaluator3 ev(s);
        const PrismRule& rule = sp.order() == 1 ? prismRule(2, 2) : prismRule(4, 3);
        double energy = 0;
        for (int e = 0; e < sp.elements(); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            const std::array<Expression, 9>& c = coef.c[std::size_t(d)];
            for (const auto& qp : rule.points) {
                ev.at(e, qp[0], qp[1], qp[2], sp.map(e, qp[0], qp[1], qp[2]).x);
                double u = 0, g[3];
                if (!ev.field(a_fi, u, g)) continue;
                double C[9];
                for (int k = 0; k < 9; ++k) C[k] = ev.eval(c[std::size_t(k)]);
                double gcg = 0;
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) gcg += g[i] * C[3 * i + j] * g[j];
                energy += qp[3] * std::abs(sp.map(e, qp[0], qp[1], qp[2]).det) * a_scale * gcg;
            }
        }
        if (a_es) s.setGlobal(p + QStringLiteral("W"), 0.5 * energy, Dim::of(2, 1, -2), tr("electric energy"));
        else s.setGlobal(p + QStringLiteral("P"), energy, Dim::power(), tr("power dissipated"));
    }
    if (a_heat) {
        if (!a_assembled) {
            // (A step in time's start, before a step: what its sources make
            // there.)
            Equations q;
            buildEquations(false, q);
            Assembled a;
            assemble(q, true, a);
            a_made = a.made;
        }
        double tmax = -INFINITY, tmin = INFINITY;
        for (int e = 0; e < sp.elements(); ++e) {
            const int d = mesh.domain[std::size_t(e)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            for (int i = 0; i < sp.perElement(); ++i) {
                const double v = f.values[std::size_t(sp.dofs(e)[i])];
                tmax = std::max(tmax, v);
                tmin = std::min(tmin, v);
            }
        }
        s.setGlobal(p + QStringLiteral("Tmax"), tmax, Dim::temperature(), tr("highest temperature"));
        s.setGlobal(p + QStringLiteral("Tmin"), tmin, Dim::temperature(), tr("lowest temperature"));
        s.setGlobal(p + QStringLiteral("P"), a_made, Dim::power(), tr("heat made by the sources"));
        if (!std::isnan(a_sink) && a_made != 0)
            s.setGlobal(p + QStringLiteral("Rth"), (tmax - a_sink) / a_made, Dim::of(-2, -1, 3, 0, 1),
                        tr("thermal resistance: (Tmax - the coolest boundary temperature) / P"));
    }
}

} // namespace

std::unique_ptr<Problem3> makeScalarProblem3(Solution3& solution, int field)
{
    return std::make_unique<ScalarProblem3>(solution, field);
}

} // namespace qucs_s::fem::detail3

namespace qucs_s::fem {

void setAssemblyParts(int parts)
{
    detail3::g_assemblyParts = std::max(0, parts);
}

int assemblyParts()
{
    return detail3::g_assemblyParts;
}

} // namespace qucs_s::fem
