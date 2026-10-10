/*
 * fem_scalar.cpp - the scalar physics: electrostatics, electric currents and
 *                  heat transfer, each -∇·(c ∇u) = f (heat in time:
 *                  ρCp ∂T/∂t too), solved for an increment of its field -
 *                  once when it is linear, by Newton's method when it reads
 *                  its own field
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_problem.h"

#include "fem_materials.h"

#include <Eigen/Dense>

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace qucs_s::fem::detail {

namespace {

/// What a boundary has: a condition of its own (the last feature's), and
/// the fluxes features add.
struct Condition {
    enum Kind { None, Dirichlet, Group };
    Kind kind = None;
    int feature = -1;      // its order among the physics' features
    Expression value;      // Dirichlet: the value
    int group = -1;        // a terminal's or floating potential's
    int terminal = -1;     // the terminal it is (voltage too)
};

struct Natural {
    Expression g;          // inward flux
    Expression q;          // q u: a Robin term (invalid: none)
    bool made = true;      // g is heat (charge) made there, not exchanged (h Text)
};

struct TerminalDef {
    QString name;
    bool floating = false; // a group of one potential
    Expression value;      // V0, or the charge (current) fed
};

class ScalarProblem : public Problem
{
public:
    ScalarProblem(Solution& solution, int field, bool vacuum = false) : Problem(solution, field), a_vacuum(vacuum) {}

    bool setup(QString* error) override;
    void initialize() override;
    bool solve(const SolveOptions& options, QString* error) override;
    void setGlobals() override;
    bool hasMass() const override { return a_heat; }
    /// The terminal sweep's matrix entry for terminal \a name alone.
    double selfMatrix(const QString& name, QString* error);

private:
    /// K, F, (mass) M and (jacobian) the rest of Newton's matrix, at the
    /// field as it is; \a mass the step's coefficient of M (0: none).
    void elementMatrices(bool jacobian, double mass, const std::vector<double>* history);
    bool buildEquations(QString* error);
    /// The values of the Dirichlet degrees of freedom at the solution's time.
    void dirichletValues();
    void assemble(bool jacobian, double mass);
    /// What is left of the equations at \a u, by equation: F (+ M history)
    /// - (K + mass M) u, the charges fed into floating groups.
    Eigen::VectorXd residual(const std::vector<double>& u, bool withSources, double mass, const std::vector<double>* history) const;
    /// K u - F at each degree of freedom (what flows out of a fixed one).
    std::vector<double> reactions(const std::vector<double>& u, bool withSources) const;
    bool sweepTerminals(const QString& solver, double tolerance, QString* error);

    bool a_vacuum;
    bool a_es = false, a_ec = false, a_heat = false;
    double a_scale = 1;                       // eps0 for electrostatics
    std::vector<Condition> a_conditions;      // by boundary
    std::vector<std::vector<Natural>> a_natural;   // by boundary
    std::vector<TerminalDef> a_terminals;
    std::vector<Expression> a_init;           // by domain: the initial value (heat)
    double a_totalSource = 0;
    double a_energy = 0;
    double a_sink = NaN;

    // The equations.
    std::vector<int> a_eq;                    // by dof: its equation, -1 none, -3 Dirichlet
    std::vector<double> a_dirichlet;          // by dof
    std::vector<int> a_dirichletBoundary;     // by dof: the boundary whose value it takes
    std::vector<int> a_dofTerminal;           // by dof: a Dirichlet or group dof's terminal
    int a_neq = 0;
    std::vector<int> a_groupEq;               // by terminal (floating): its equation
    std::vector<double> a_K, a_M, a_J, a_F;   // element matrices (and the Jacobian's other part), by element
    struct EdgeTerm {
        int edge;
        double K[9], J[9], F[3];
        double made = 0;   // what its sources make (not a film's exchange)
    };
    std::vector<EdgeTerm> a_edges;
    LinearSystem a_system;
    bool a_patternReady = false;
    // What the factorization is of: none yet, K + mass M (+ J).
    bool a_factorized = false;
    double a_factorMass = -1;
    bool a_factorJacobian = false;
    bool a_matrixVaries = true;               // a coefficient reads a field or t
    bool a_sourcesVary = true;                // a source, a flux reads a field or t
};

// ------------------------------------------------------------------ Setup

bool ScalarProblem::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field& f = field();
    const Model& model = a_s.model();
    const Topology& topo = a_s.topology();
    const Mesh& mesh = a_s.mesh();
    const QString type = a_node->type;
    a_es = type == QLatin1String("electrostatics");
    a_ec = type == QLatin1String("currents");
    a_heat = type == QLatin1String("heat");
    a_scale = a_es ? Eps0 : 1.0;
    QString why;
    // Its domains.
    const QVector<int> domains = resolveSelection(topo, model, Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(topo.domains.size(), 0);
    for (int d : domains) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    const std::vector<const Node*> material = materials();
    // The domain features: the model's (last wins), the sources (they add).
    const QString modelFeature = a_es ? QStringLiteral("electrostatics/charge") : a_ec ? QStringLiteral("currents/conservation") : QStringLiteral("heat/solid");
    const QString property = a_es ? QStringLiteral("epsilonr") : a_ec ? QStringLiteral("sigma") : QStringLiteral("k");
    const QString propertyName = a_es ? tr("relative permittivity") : a_ec ? tr("electrical conductivity") : tr("thermal conductivity");
    std::vector<const Node*> modelOf(topo.domains.size(), nullptr);
    Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    coef.c.assign(topo.domains.size(), Expression());
    coef.sources.assign(topo.domains.size(), {});
    coef.sourceScale.assign(topo.domains.size(), 1.0);
    coef.capacity.assign(topo.domains.size(), Expression());
    a_conditions.assign(topo.boundaries.size(), Condition());
    a_natural.assign(topo.boundaries.size(), {});
    a_terminals.clear();
    a_init.assign(topo.domains.size(), Expression());
    // The volume of each domain (m³; about the axis, round it), for a total
    // power spread over some.
    std::vector<double> volume(topo.domains.size(), 0);
    {
        const Space& sp = *f.space;
        const TriangleRule& rule = triangleRule(2);
        for (int t = 0; t < sp.elements(); ++t)
            for (const auto& q : rule.points) {
                const Space::Map m = sp.map(t, q[0], q[1]);
                volume[std::size_t(mesh.domain[std::size_t(t)])] += q[2] * std::abs(m.det) * a_s.weight(m.x);
            }
    }
    int order = 0;
    for (const Node& feature : a_node->children) {
        ++order;
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k) continue;
        const QVector<int> picked = resolveSelection(topo, model, k->selection, feature.selection(), &why);
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
                    if (f.domains[std::size_t(d)]) total += volume[std::size_t(d)];
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
                coef.sourceScale[std::size_t(d)] = scale;   // (one total power a domain)
            }
            continue;
        }
        if (k->selection != Level::Boundary) continue;
        // Boundary features: those with a condition of their own (last wins),
        // and fluxes (they add).
        const QString t = feature.type;
        const bool exclusive = t.endsWith(QLatin1String("/zerocharge")) || t.endsWith(QLatin1String("/insulation"))
                               || t.endsWith(QLatin1String("/ground")) || t.endsWith(QLatin1String("/potential"))
                               || t.endsWith(QLatin1String("/terminal")) || t.endsWith(QLatin1String("/floating"))
                               || t == QLatin1String("heat/temperature");
        if (exclusive) {
            Condition c;
            c.feature = order;
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
                if (term.value.isValid() && !term.value.isConstant() && term.floating)
                    problem(tr("%1 of %2 must be a constant").arg(key, feature.name()));
                c.terminal = int(a_terminals.size());
                if (term.floating) {
                    c.kind = Condition::Group;
                    c.group = c.terminal;
                } else {
                    c.kind = Condition::Dirichlet;
                    c.value = term.value;
                }
                a_terminals.push_back(term);
            }
            for (int b : picked) a_conditions[std::size_t(b)] = c;
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
            // εσ(Tamb⁴ - T⁴) = h_r (Tamb - T), h_r = εσ(Tamb² + T²)(Tamb + T):
            // a film of h_r (of T itself).
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
        for (int b : picked) a_natural[std::size_t(b)].push_back(n);
    }
    // Each domain's coefficient: its feature's own, or its material's.
    for (int d = 0; d < int(topo.domains.size()); ++d) {
        if (!f.domains[std::size_t(d)]) continue;
        const Node* feat = modelOf[std::size_t(d)];
        if (!feat) {
            problem(tr("domain %1 has no %2 feature").arg(d + 1).arg(modelFeature.section(QLatin1Char('/'), 1)));
            continue;
        }
        const bool user = feat->text(QStringLiteral("source")) == QLatin1String("user");
        const Node* m = material[std::size_t(d)];
        const QString object = topo.objectNames.value(topo.domains[std::size_t(d)].owner);
        auto from = [&](const QString& key, const QString& name, QString* text) -> const Node* {
            if (user) {
                *text = feat->text(key);
                return feat;
            }
            if (!m) {
                problem(tr("domain %1 (%2) has no material: its %3 is needed").arg(d + 1).arg(object, name));
                return nullptr;
            }
            *text = materialProperty(*m, key);
            if (text->trimmed().isEmpty()) {
                problem(tr("domain %1 (%2): its material, %3, has no %4").arg(d + 1).arg(object, m->name(), name));
                return nullptr;
            }
            return m;
        };
        QString text;
        const Node* where = feat;
        if (a_vacuum) text = QStringLiteral("1");
        else where = from(property, propertyName, &text);
        if (!where) continue;
        Expression e = compile(text, *where, property);
        if (e.isValid() && e.isConstant() && !(e.constant() > 0) && !(a_es && e.constant() != 0))
            problem(tr("domain %1: its %2 must be more than 0").arg(d + 1).arg(propertyName));
        coef.c[std::size_t(d)] = e;
        if (a_heat) {
            // ρ Cp, for a step in time (said only then: a stationary study
            // needs neither).
            QString rho, cp;
            const Node* r = user ? feat : m;
            if (r) {
                rho = user ? feat->text(QStringLiteral("rho")) : materialProperty(*m, QStringLiteral("rho"));
                cp = user ? feat->text(QStringLiteral("Cp")) : materialProperty(*m, QStringLiteral("Cp"));
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
    // Whether its matrix changes with the fields or time.
    a_matrixVaries = false;
    for (const Expression& e : coef.c) a_matrixVaries = a_matrixVaries || (e.isValid() && a_s.varies(e));
    for (const Expression& e : coef.capacity) a_matrixVaries = a_matrixVaries || (e.isValid() && a_s.varies(e));
    for (const auto& list : a_natural)
        for (const Natural& n : list) a_matrixVaries = a_matrixVaries || (n.q.isValid() && a_s.varies(n.q));
    a_sourcesVary = false;
    for (const auto& list : coef.sources)
        for (const Expression& e : list) a_sourcesVary = a_sourcesVary || a_s.varies(e);
    for (const auto& list : a_natural)
        for (const Natural& n : list) a_sourcesVary = a_sourcesVary || (n.g.isValid() && a_s.varies(n.g));
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    a_patternReady = false;
    a_factorized = false;
    return true;
}

void ScalarProblem::initialize()
{
    Field& f = field();
    const Space& sp = *f.space;
    const Mesh& mesh = a_s.mesh();
    f.values.assign(std::size_t(sp.size()), f.initial);
    if (!a_heat) return;
    std::vector<double> values(std::size_t(a_s.pointScope().slotCount()), NaN);
    const auto& defs = a_s.definitions();
    for (int t = 0; t < sp.elements(); ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || d >= int(a_init.size()) || !a_init[std::size_t(d)].isValid()) continue;
        const Expression& e = a_init[std::size_t(d)];
        for (int i = 0; i < sp.perElement(); ++i) {
            const int dof = sp.dofs(t)[i];
            double v = e.isConstant() ? e.constant() : NaN;
            if (!e.isConstant()) {
                const QPointF p = sp.point(dof);
                for (int s = 0; s < int(defs.size()); ++s) {
                    const auto k = defs[std::size_t(s)].kind;
                    values[std::size_t(s)] = k == Solution::VariableDef::X ? p.x() : k == Solution::VariableDef::Y ? p.y()
                                             : k == Solution::VariableDef::Time                                 ? a_s.time()
                                                                                                                 : NaN;
                }
                v = e.eval(values.data());
            }
            if (std::isfinite(v)) f.values[std::size_t(dof)] = v;
        }
    }
}

// ------------------------------------------------------------------ Assembly

void ScalarProblem::elementMatrices(bool jacobian, double massCoef, const std::vector<double>* history)
{
    const bool mass = massCoef != 0;
    Field& fld = field();
    const Space& sp = *fld.space;
    const Mesh& mesh = a_s.mesh();
    const Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const int per = sp.perElement();
    const int nt = sp.elements();
    const std::size_t pp = std::size_t(per * per);
    a_K.assign(std::size_t(nt) * pp, 0);
    a_F.assign(std::size_t(nt) * std::size_t(per), 0);
    a_M.assign(mass ? std::size_t(nt) * pp : 0, 0);
    a_J.assign(jacobian ? std::size_t(nt) * pp : 0, 0);
    const bool selfReading = jacobian && a_reads.contains(a_fi);
    parallelFor(nt, [&](int begin, int end) {
        PointEvaluator ev(a_s);
        for (int t = begin; t < end; ++t) {
            const int dom = mesh.domain[std::size_t(t)];
            if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
            const Expression& c = coef.c[std::size_t(dom)];
            const Expression& cap = coef.capacity[std::size_t(dom)];
            const std::vector<Expression>& sources = coef.sources[std::size_t(dom)];
            const double sourceScale = coef.sourceScale[std::size_t(dom)];
            bool constant = c.isConstant() && (!mass || cap.isConstant());
            for (const Expression& s : sources) constant = constant && s.isConstant();
            const int degree = sp.curved(t) || a_s.axisymmetric() ? 5 : constant ? (sp.order() == 1 ? 2 : 4) : (sp.order() == 1 ? 2 : 4);
            const TriangleRule& rule = triangleRule(degree);
            double* K = &a_K[std::size_t(t) * pp];
            double* F = &a_F[std::size_t(t) * std::size_t(per)];
            double* M = mass ? &a_M[std::size_t(t) * pp] : nullptr;
            double* J = jacobian ? &a_J[std::size_t(t) * pp] : nullptr;
            const int* dofs = sp.dofs(t);
            Shape s;
            double gx[6], gy[6];
            for (const auto& q : rule.points) {
                shapeAt(sp.order(), q[0], q[1], s);
                const Space::Map m = sp.map(t, q[0], q[1]);
                const double w = q[2] * std::abs(m.det) * a_s.weight(m.x);
                double cv = 0, fv = 0, capv = 0, dc = 0, df = 0, dcap = 0;
                if (constant && !selfReading) {
                    cv = c.constant();
                    for (const Expression& src : sources) fv += src.constant();
                    if (mass) capv = cap.constant();
                } else {
                    ev.moveTo(t, q[0], q[1]);
                    cv = ev.eval(c);
                    for (const Expression& src : sources) fv += ev.eval(src);
                    if (mass) capv = ev.eval(cap);
                    if (selfReading) {
                        // Their derivatives in the field's value, numerically.
                        double u = 0, ux = 0, uy = 0;
                        ev.field(a_fi, u, ux, uy);
                        const double du = 1e-7 * (std::abs(u) + 1);
                        ev.perturb(a_fi, du);
                        double c2 = ev.eval(c), f2 = 0, cap2 = mass ? ev.eval(cap) : 0;
                        for (const Expression& src : sources) f2 += ev.eval(src);
                        ev.perturb(a_fi, 0);
                        dc = (c2 - cv) / du;
                        df = (f2 - fv) / du;
                        dcap = mass ? (cap2 - capv) / du : 0;
                    }
                }
                cv *= a_scale;
                dc *= a_scale;
                fv *= sourceScale;
                df *= sourceScale;
                for (int i = 0; i < per; ++i) Space::gradient(m, s.dxi[i], s.deta[i], gx[i], gy[i]);
                for (int i = 0; i < per; ++i) {
                    for (int j = i; j < per; ++j) {
                        const double v = w * cv * (gx[i] * gx[j] + gy[i] * gy[j]);
                        K[i * per + j] += v;
                        if (j != i) K[j * per + i] += v;
                        if (mass) {
                            const double mv = w * capv * s.n[i] * s.n[j];
                            M[i * per + j] += mv;
                            if (j != i) M[j * per + i] += mv;
                        }
                    }
                    F[i] += w * fv * s.n[i];
                }
                if (selfReading) {
                    // (K u - F)' = K + dc N_j ∇u·∇N_i - df N_j N_i (+ the mass term's)
                    double ux = 0, uy = 0, upt = 0, hpt = 0;
                    for (int k = 0; k < per; ++k) {
                        const double v = fld.values[std::size_t(dofs[k])];
                        ux += gx[k] * v;
                        uy += gy[k] * v;
                        upt += s.n[k] * v;
                        if (history) hpt += s.n[k] * (*history)[std::size_t(dofs[k])];
                    }
                    // The mass term's: d/du_j of cap(u) (a u - h) N_i.
                    const double massTerm = mass ? dcap * (massCoef * upt - hpt) : 0;
                    for (int i = 0; i < per; ++i)
                        for (int j = 0; j < per; ++j) {
                            const double v = dc * s.n[j] * (ux * gx[i] + uy * gy[i]) - df * s.n[j] * s.n[i] + massTerm * s.n[j] * s.n[i];
                            J[i * per + j] += w * v;
                        }
                }
            }
        }
    });
    // The boundaries' fluxes and Robin terms, edge by edge.
    a_edges.clear();
    PointEvaluator ev(a_s);
    const int lineN = sp.order() + 1 + (a_s.axisymmetric() ? 1 : 0);
    for (int e = 0; e < int(mesh.edges.size()); ++e) {
        const Mesh::Edge& me = mesh.edges[std::size_t(e)];
        const std::vector<Natural>& nat = a_natural[std::size_t(me.boundary)];
        if (nat.empty()) continue;
        // On a domain of this physics.
        int tri = -1;
        for (int side : {me.left, me.right})
            if (side >= 0 && fld.domains[std::size_t(mesh.domain[std::size_t(side)])]) {
                tri = side;
                break;
            }
        if (tri < 0) continue;
        const std::array<int, 3>& tv = mesh.triangles[std::size_t(tri)];
        const int k = Space::localEdge(tv, me.a, me.b);
        const bool same = tv[std::size_t(k)] == me.a;
        EdgeTerm term{e, {}, {}, {}};
        const int ne = sp.order() == 1 ? 2 : 3;
        const std::array<int, 3> edofs = sp.edgeDofs(e);
        for (const auto& q : lineRule(lineN)) {
            double jac = 0;
            const QPointF x = sp.edgePoint(e, q[0], &jac);
            // Its reference point in the triangle, for fields there.
            const double s = same ? q[0] : 1 - q[0];
            double xi = 0, eta = 0;
            if (k == 0) {
                xi = s;
                eta = 0;
            } else if (k == 1) {
                xi = 1 - s;
                eta = s;
            } else {
                xi = 0;
                eta = 1 - s;
            }
            ev.moveTo(tri, xi, eta);
            double g = 0, qq = 0, made = 0, dg = 0, dq = 0;
            auto evaluate = [&](double& gg, double& qv, double* madeOut) {
                gg = 0;
                qv = 0;
                for (const Natural& n : nat) {
                    if (n.g.isValid()) {
                        const double v = ev.eval(n.g);
                        gg += v;
                        if (madeOut && n.made) *madeOut += v;
                    }
                    if (n.q.isValid()) qv += ev.eval(n.q);
                }
            };
            evaluate(g, qq, &made);
            if (selfReading) {
                double u = 0, ux = 0, uy = 0;
                ev.field(a_fi, u, ux, uy);
                const double du = 1e-7 * (std::abs(u) + 1);
                ev.perturb(a_fi, du);
                double g2 = 0, q2 = 0;
                evaluate(g2, q2, nullptr);
                ev.perturb(a_fi, 0);
                dg = (g2 - g) / du;
                dq = (q2 - qq) / du;
            }
            const double sq = q[0];
            double N[3];
            if (ne == 2) {
                N[0] = 1 - sq;
                N[1] = sq;
            } else {
                N[0] = (1 - sq) * (1 - 2 * sq);
                N[1] = sq * (2 * sq - 1);
                N[2] = 4 * sq * (1 - sq);
            }
            const double w = q[1] * jac * a_s.weight(x);
            term.made += w * made;
            double upt = 0;
            if (selfReading)
                for (int i = 0; i < ne; ++i) upt += N[i] * fld.values[std::size_t(edofs[std::size_t(i)])];
            for (int i = 0; i < ne; ++i) {
                term.F[i] += w * g * N[i];
                for (int j = 0; j < ne; ++j) {
                    term.K[i * 3 + j] += w * qq * N[i] * N[j];
                    // (K u - F)' beyond K: (u dq - dg) N_i N_j
                    if (selfReading) term.J[i * 3 + j] += w * (upt * dq - dg) * N[i] * N[j];
                }
            }
        }
        a_edges.push_back(term);
    }
}

bool ScalarProblem::buildEquations(QString* error)
{
    Field& fld = field();
    const Space& sp = *fld.space;
    const Mesh& mesh = a_s.mesh();
    const int nd = sp.size();
    const int per = sp.perElement();
    a_eq.assign(std::size_t(nd), -1);
    a_dofTerminal.assign(std::size_t(nd), -1);
    std::vector<int> group(std::size_t(nd), -1);
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = mesh.domain[std::size_t(t)];
        if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
        for (int i = 0; i < per; ++i) a_eq[std::size_t(sp.dofs(t)[i])] = -2;
    }
    // The boundaries' own conditions, the latest feature's last: a point
    // two share takes the later's.
    std::vector<int> byOrder;
    for (int b = 0; b < int(a_conditions.size()); ++b)
        if (a_conditions[std::size_t(b)].kind != Condition::None) byOrder.push_back(b);
    std::stable_sort(byOrder.begin(), byOrder.end(), [&](int a, int b) {
        return a_conditions[std::size_t(a)].feature < a_conditions[std::size_t(b)].feature;
    });
    std::vector<std::vector<int>> edgesOf(a_conditions.size());
    for (int e = 0; e < int(mesh.edges.size()); ++e) edgesOf[std::size_t(mesh.edges[std::size_t(e)].boundary)].push_back(e);
    a_dirichletBoundary.assign(std::size_t(nd), -1);
    bool anyFixed = false;
    for (int b : byOrder) {
        const Condition& c = a_conditions[std::size_t(b)];
        for (int e : edgesOf[std::size_t(b)]) {
            const std::array<int, 3> dofs = sp.edgeDofs(e);
            for (int dof : dofs) {
                if (dof < 0 || a_eq[std::size_t(dof)] == -1) continue;
                if (c.kind == Condition::Dirichlet) {
                    a_eq[std::size_t(dof)] = -3;
                    a_dirichletBoundary[std::size_t(dof)] = b;
                    group[std::size_t(dof)] = -1;
                    anyFixed = true;
                } else {
                    a_eq[std::size_t(dof)] = -4;
                    group[std::size_t(dof)] = c.group;
                    a_dirichletBoundary[std::size_t(dof)] = -1;
                }
                a_dofTerminal[std::size_t(dof)] = c.terminal;
            }
        }
    }
    bool robin = false;
    for (const auto& list : a_natural)
        for (const Natural& n : list) robin = robin || n.q.isValid();
    if (!anyFixed && !robin) {
        *error = a_heat ? tr("%1: no temperature is set and no heat leaves (no Temperature, no convective Heat Flux): the "
                             "temperature is not determined").arg(a_node->name())
                        : tr("%1: no potential is set (no Ground, Electric Potential or Terminal at a voltage): the "
                             "potential is not determined").arg(a_node->name());
        return false;
    }
    // Numbered: the free degrees of freedom, then one for each group.
    a_neq = 0;
    for (int dof = 0; dof < nd; ++dof)
        if (a_eq[std::size_t(dof)] == -2) a_eq[std::size_t(dof)] = a_neq++;
    a_groupEq.assign(a_terminals.size(), -1);
    for (int g = 0; g < int(a_terminals.size()); ++g)
        if (a_terminals[std::size_t(g)].floating) a_groupEq[std::size_t(g)] = a_neq++;
    for (int dof = 0; dof < nd; ++dof)
        if (a_eq[std::size_t(dof)] == -4) a_eq[std::size_t(dof)] = a_groupEq[std::size_t(group[std::size_t(dof)])];
    if (a_neq == 0) {
        *error = tr("%1: nothing to solve - every value is set").arg(a_node->name());
        return false;
    }
    // The pattern of the matrix.
    a_system.setPattern(a_neq, [&](const std::function<void(const int*, int)>& element) {
        int local[6];
        for (int t = 0; t < sp.elements(); ++t) {
            const int dom = mesh.domain[std::size_t(t)];
            if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
            for (int i = 0; i < per; ++i) local[i] = a_eq[std::size_t(sp.dofs(t)[i])];
            element(local, per);
        }
    });
    a_patternReady = true;
    a_factorized = false;
    return true;
}

void ScalarProblem::dirichletValues()
{
    const Space& sp = *field().space;
    const int nd = sp.size();
    a_dirichlet.assign(std::size_t(nd), NaN);
    std::vector<double> values(std::size_t(a_s.pointScope().slotCount()), NaN);
    const auto& defs = a_s.definitions();
    for (int dof = 0; dof < nd; ++dof) {
        const int b = a_dirichletBoundary[std::size_t(dof)];
        if (a_eq[std::size_t(dof)] != -3 || b < 0) continue;
        const Expression& e = a_conditions[std::size_t(b)].value;
        double v = NaN;
        if (e.isConstant()) v = e.constant();
        else if (e.isValid()) {
            // Of x, y and t alone.
            const QPointF p = sp.point(dof);
            for (int s = 0; s < int(defs.size()); ++s) {
                const auto k = defs[std::size_t(s)].kind;
                values[std::size_t(s)] = k == Solution::VariableDef::X ? p.x() : k == Solution::VariableDef::Y ? p.y()
                                         : k == Solution::VariableDef::Time                                 ? a_s.time()
                                                                                                             : NaN;
            }
            v = e.eval(values.data());
        }
        a_dirichlet[std::size_t(dof)] = v;
    }
}

void ScalarProblem::assemble(bool jacobian, double mass)
{
    const Space& sp = *field().space;
    const int per = sp.perElement();
    const std::size_t pp = std::size_t(per * per);
    const Mesh& mesh = a_s.mesh();
    a_system.clear();
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = mesh.domain[std::size_t(t)];
        if (dom < 0 || !field().domains[std::size_t(dom)]) continue;
        const double* K = &a_K[std::size_t(t) * pp];
        const double* M = mass != 0 && !a_M.empty() ? &a_M[std::size_t(t) * pp] : nullptr;
        const double* J = jacobian && !a_J.empty() ? &a_J[std::size_t(t) * pp] : nullptr;
        const int* dofs = sp.dofs(t);
        for (int i = 0; i < per; ++i) {
            const int qi = a_eq[std::size_t(dofs[i])];
            if (qi < 0) continue;
            for (int j = 0; j < per; ++j) {
                const int qj = a_eq[std::size_t(dofs[j])];
                if (qj < 0) continue;
                double v = K[i * per + j];
                if (M) v += mass * M[i * per + j];
                if (J) v += J[i * per + j];
                a_system.add(qi, qj, v);
            }
        }
    }
    const int ne = sp.order() == 1 ? 2 : 3;
    for (const EdgeTerm& et : a_edges) {
        const std::array<int, 3> dofs = sp.edgeDofs(et.edge);
        for (int i = 0; i < ne; ++i) {
            const int qi = a_eq[std::size_t(dofs[std::size_t(i)])];
            if (qi < 0) continue;
            for (int j = 0; j < ne; ++j) {
                const int qj = a_eq[std::size_t(dofs[std::size_t(j)])];
                if (qj >= 0) a_system.add(qi, qj, et.K[i * 3 + j] + (jacobian ? et.J[i * 3 + j] : 0));
            }
        }
    }
}

Eigen::VectorXd ScalarProblem::residual(const std::vector<double>& u, bool withSources, double mass, const std::vector<double>* history) const
{
    const Space& sp = *a_s.fields()[std::size_t(a_fi)].space;
    const int per = sp.perElement();
    const std::size_t pp = std::size_t(per * per);
    const Mesh& mesh = a_s.mesh();
    const Field& fld = a_s.fields()[std::size_t(a_fi)];
    Eigen::VectorXd r = Eigen::VectorXd::Zero(a_neq);
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = mesh.domain[std::size_t(t)];
        if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
        const double* K = &a_K[std::size_t(t) * pp];
        const double* F = &a_F[std::size_t(t) * std::size_t(per)];
        const double* M = mass != 0 && !a_M.empty() ? &a_M[std::size_t(t) * pp] : nullptr;
        const int* dofs = sp.dofs(t);
        for (int i = 0; i < per; ++i) {
            const int qi = a_eq[std::size_t(dofs[i])];
            if (qi < 0) continue;
            double v = withSources ? F[i] : 0;
            for (int j = 0; j < per; ++j) {
                const double uj = u[std::size_t(dofs[j])];
                v -= K[i * per + j] * uj;
                if (M) v += M[i * per + j] * ((history ? (*history)[std::size_t(dofs[j])] : 0) - mass * uj);
            }
            r[qi] += v;
        }
    }
    const int ne = sp.order() == 1 ? 2 : 3;
    for (const EdgeTerm& et : a_edges) {
        const std::array<int, 3> dofs = sp.edgeDofs(et.edge);
        for (int i = 0; i < ne; ++i) {
            const int qi = a_eq[std::size_t(dofs[std::size_t(i)])];
            if (qi < 0) continue;
            double v = withSources ? et.F[i] : 0;
            for (int j = 0; j < ne; ++j) v -= et.K[i * 3 + j] * u[std::size_t(dofs[std::size_t(j)])];
            r[qi] += v;
        }
    }
    if (withSources)
        for (int g = 0; g < int(a_terminals.size()); ++g)
            if (a_groupEq[std::size_t(g)] >= 0 && a_terminals[std::size_t(g)].value.isConstant())
                r[a_groupEq[std::size_t(g)]] += a_terminals[std::size_t(g)].value.constant();
    return r;
}

std::vector<double> ScalarProblem::reactions(const std::vector<double>& u, bool withSources) const
{
    // K u - F at the fixed degrees of freedom: what flows out there.
    const Space& sp = *a_s.fields()[std::size_t(a_fi)].space;
    const Field& fld = a_s.fields()[std::size_t(a_fi)];
    const int per = sp.perElement();
    std::vector<double> r(std::size_t(sp.size()), 0);
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = a_s.mesh().domain[std::size_t(t)];
        if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
        const double* K = &a_K[std::size_t(t) * std::size_t(per * per)];
        const double* F = &a_F[std::size_t(t) * std::size_t(per)];
        const int* dofs = sp.dofs(t);
        for (int i = 0; i < per; ++i) {
            if (a_eq[std::size_t(dofs[i])] != -3) continue;
            double v = withSources ? -F[i] : 0;
            for (int j = 0; j < per; ++j) v += K[i * per + j] * u[std::size_t(dofs[j])];
            r[std::size_t(dofs[i])] += v;
        }
    }
    const int ne = sp.order() == 1 ? 2 : 3;
    for (const EdgeTerm& et : a_edges) {
        const std::array<int, 3> dofs = sp.edgeDofs(et.edge);
        for (int i = 0; i < ne; ++i) {
            if (a_eq[std::size_t(dofs[std::size_t(i)])] != -3) continue;
            double v = withSources ? -et.F[i] : 0;
            for (int j = 0; j < ne; ++j) v += et.K[i * 3 + j] * u[std::size_t(dofs[std::size_t(j)])];
            r[std::size_t(dofs[std::size_t(i)])] += v;
        }
    }
    return r;
}

// ------------------------------------------------------------------ Solving

bool ScalarProblem::solve(const SolveOptions& o, QString* error)
{
    Field& fld = field();
    const Space& sp = *fld.space;
    const bool mass = o.mass != 0 && a_heat;
    if (mass)
        for (int d = 0; d < int(fld.domains.size()); ++d)
            if (fld.domains[std::size_t(d)] && !a_s.coefficients()[std::size_t(a_fi)].capacity[std::size_t(d)].isValid()) {
                *error = tr("%1: domain %2 has no density and heat capacity (rho, Cp): a step in time needs them")
                             .arg(a_node->name())
                             .arg(d + 1);
                return false;
            }
    const bool selfReading = a_reads.contains(a_fi);
    const bool newton = selfReading && o.nonlinear != QLatin1String("picard");
    if (!a_patternReady && !buildEquations(error)) return false;
    dirichletValues();
    // The values to start from: the field's own, the fixed ones as set.
    std::vector<double> u = fld.values;
    if (int(u.size()) != sp.size()) u.assign(std::size_t(sp.size()), fld.initial);
    for (int dof = 0; dof < sp.size(); ++dof) {
        if (a_eq[std::size_t(dof)] == -3) u[std::size_t(dof)] = a_dirichlet[std::size_t(dof)];
        else if (a_eq[std::size_t(dof)] == -1) u[std::size_t(dof)] = fld.initial;
    }
    // A group's values one (they start alike).
    for (int g = 0; g < int(a_terminals.size()); ++g) {
        if (a_groupEq[std::size_t(g)] < 0) continue;
        double first = NaN;
        for (int dof = 0; dof < sp.size(); ++dof)
            if (a_eq[std::size_t(dof)] == a_groupEq[std::size_t(g)]) {
                if (std::isnan(first)) first = u[std::size_t(dof)];
                u[std::size_t(dof)] = first;
            }
    }
    fld.values = u;
    const double m = mass ? o.mass : 0.0;
    const int iterations = newton ? std::max(1, o.maxIterations) : 1;
    bool converged = !newton;
    for (int it = 0; it < iterations; ++it) {
        // Element matrices made again where something in them changes (a
        // coefficient or source of the fields or of t, Newton's); the
        // factorization where the matrix does.
        const bool have = !a_K.empty() && (!mass || !a_M.empty());
        if (!have || a_matrixVaries || a_sourcesVary || newton) elementMatrices(newton, m, o.history);
        if (!a_factorized || a_matrixVaries || newton || a_factorJacobian || a_factorMass != m) {
            assemble(newton, m);
            if (!a_system.factorize(!newton, o.solver, o.tolerance, a_node->name(), &a_s.log, error)) return false;
            a_factorized = true;
            a_factorMass = m;
            a_factorJacobian = newton;
        }
        const Eigen::VectorXd r = residual(u, true, m, o.history);
        Eigen::VectorXd delta;
        if (!a_system.solve(r, delta, a_node->name(), error)) return false;
        double change = 0, size = 0;
        for (int dof = 0; dof < sp.size(); ++dof) {
            const int q = a_eq[std::size_t(dof)];
            if (q < 0) continue;
            u[std::size_t(dof)] += delta[q];
            change = std::max(change, std::abs(delta[q]));
            size = std::max(size, std::abs(u[std::size_t(dof)]));
        }
        fld.values = u;
        if (newton) {
            if (change <= o.tolerance * (size + 1e-30) || change == 0) {
                converged = true;
                a_s.log << tr("%1: Newton converged in %2 steps").arg(a_node->name()).arg(it + 1);
                break;
            }
        }
    }
    if (!converged) a_s.log << tr("Warning: %1: Newton's method did not converge in %2 steps").arg(a_node->name()).arg(iterations);
    // Not solved: its initial value there (another physics may read it).
    for (double& v : fld.values)
        if (std::isnan(v)) v = fld.initial;
    fld.solved = true;
    // What flows out of each terminal: its charge (current); K and F at the
    // values found, after Newton's steps.
    if (newton) elementMatrices(false, m, o.history);
    const std::vector<double> rr = reactions(u, true);
    fld.terminals.clear();
    for (int g = 0; g < int(a_terminals.size()); ++g) {
        const TerminalDef& term = a_terminals[std::size_t(g)];
        Field::Terminal out;
        out.name = term.name;
        out.floating = term.floating;
        double charge = 0, potential = NaN;
        for (int dof = 0; dof < int(u.size()); ++dof) {
            if (a_dofTerminal[std::size_t(dof)] != g) continue;
            if (term.floating) potential = u[std::size_t(dof)];
            else {
                charge += rr[std::size_t(dof)];
                potential = u[std::size_t(dof)];
            }
        }
        if (term.floating) charge = term.value.isConstant() ? term.value.constant() : NaN;
        out.charge = charge;
        out.potential = potential;
        fld.terminals.push_back(out);
    }
    // The energy (es), the power (ec): u K u over the elements.
    double energy = 0;
    const int per = sp.perElement();
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = a_s.mesh().domain[std::size_t(t)];
        if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
        const double* K = &a_K[std::size_t(t) * std::size_t(per * per)];
        const int* dofs = sp.dofs(t);
        for (int i = 0; i < per; ++i)
            for (int j = 0; j < per; ++j) energy += u[std::size_t(dofs[i])] * K[i * per + j] * u[std::size_t(dofs[j])];
    }
    a_energy = energy;
    // The heat made: the sources' integral (F's sum) and the boundaries'
    // (not what a film exchanges with the outside).
    double total = 0;
    for (double f : a_F) total += f;
    for (const EdgeTerm& et : a_edges) total += et.made;
    a_totalSource = total;
    fld.matrix.clear();
    if (o.sweep && !sweepTerminals(o.solver, o.tolerance, error)) return false;
    return true;
}

bool ScalarProblem::sweepTerminals(const QString& solver, double tolerance, QString* error)
{
    // Each voltage terminal at 1 V, the others and every other fixed value
    // at 0, no sources: K alone, factorized.
    Field& fld = field();
    std::vector<int> voltage;
    for (int g = 0; g < int(a_terminals.size()); ++g)
        if (!a_terminals[std::size_t(g)].floating) voltage.push_back(g);
    if (voltage.empty()) return true;
    if (!a_factorized || a_factorMass != 0 || a_factorJacobian || a_matrixVaries) {
        assemble(false, 0);
        if (!a_system.factorize(true, solver, tolerance, a_node->name(), &a_s.log, error)) return false;
        a_factorized = true;
        a_factorMass = 0;
        a_factorJacobian = false;
    }
    const int n = int(voltage.size());
    fld.matrix.assign(std::size_t(n), std::vector<double>(std::size_t(n), 0));
    const int nd = fld.space->size();
    for (int j = 0; j < n; ++j) {
        std::vector<double> u(std::size_t(nd), 0);
        for (int dof = 0; dof < nd; ++dof)
            if (a_eq[std::size_t(dof)] == -3) u[std::size_t(dof)] = a_dofTerminal[std::size_t(dof)] == voltage[std::size_t(j)] ? 1 : 0;
        Eigen::VectorXd delta;
        if (!a_system.solve(residual(u, false, 0, nullptr), delta, a_node->name(), error)) return false;
        for (int dof = 0; dof < nd; ++dof)
            if (a_eq[std::size_t(dof)] >= 0) u[std::size_t(dof)] += delta[a_eq[std::size_t(dof)]];
        const std::vector<double> rj = reactions(u, false);
        for (int i = 0; i < n; ++i) {
            double q = 0;
            for (int dof = 0; dof < nd; ++dof)
                if (a_dofTerminal[std::size_t(dof)] == voltage[std::size_t(i)] && a_eq[std::size_t(dof)] == -3) q += rj[std::size_t(dof)];
            fld.matrix[std::size_t(i)][std::size_t(j)] = q;
        }
    }
    // Its terminals' names in the matrix's order.
    std::vector<Field::Terminal> ordered;
    for (int g : voltage) ordered.push_back(fld.terminals[std::size_t(g)]);
    for (int g = 0; g < int(a_terminals.size()); ++g)
        if (a_terminals[std::size_t(g)].floating) ordered.push_back(fld.terminals[std::size_t(g)]);
    fld.terminals = ordered;
    return true;
}

double ScalarProblem::selfMatrix(const QString& name, QString* error)
{
    // Assembled without sources; one terminal at 1 V, every other fixed
    // value at 0; its charge.
    elementMatrices(false, 0, nullptr);
    if (!buildEquations(error)) return NaN;
    assemble(false, 0);
    if (!a_system.factorize(true, QStringLiteral("direct"), 1e-9, a_node->name(), &a_s.log, error)) return NaN;
    int g = -1;
    for (int i = 0; i < int(a_terminals.size()); ++i)
        if (a_terminals[std::size_t(i)].name == name && !a_terminals[std::size_t(i)].floating) g = i;
    if (g < 0) {
        *error = tr("%1 has no terminal %2 at a voltage").arg(a_node->name(), name);
        return NaN;
    }
    const int nd = field().space->size();
    std::vector<double> u(std::size_t(nd), 0);
    for (int dof = 0; dof < nd; ++dof)
        if (a_eq[std::size_t(dof)] == -3) u[std::size_t(dof)] = a_dofTerminal[std::size_t(dof)] == g ? 1 : 0;
    Eigen::VectorXd delta;
    if (!a_system.solve(residual(u, false, 0, nullptr), delta, a_node->name(), error)) return NaN;
    for (int dof = 0; dof < nd; ++dof)
        if (a_eq[std::size_t(dof)] >= 0) u[std::size_t(dof)] += delta[a_eq[std::size_t(dof)]];
    const std::vector<double> r = reactions(u, false);
    double q = 0;
    for (int dof = 0; dof < nd; ++dof)
        if (a_dofTerminal[std::size_t(dof)] == g && a_eq[std::size_t(dof)] == -3) q += r[std::size_t(dof)];
    return q;
}

// ------------------------------------------------------------------ Globals

QString matrixName(const QString& tag, const QString& letter, const QString& a, const QString& b)
{
    return tag + QLatin1Char('.') + letter + QLatin1Char('_') + a + QLatin1Char('_') + b;
}

void ScalarProblem::setGlobals()
{
    Solution& s = a_s;
    Field& f = field();
    const QString p = f.tag + QLatin1Char('.');
    const Dim charge = a_es ? Dim::charge() : Dim::current();
    const QString q = a_es ? QStringLiteral("Q") : QStringLiteral("I");
    for (const Field::Terminal& t : f.terminals) {
        if (a_heat) break;
        s.setGlobal(p + QStringLiteral("V_") + t.name, t.potential, Dim::voltage(), tr("terminal %1's potential").arg(t.name));
        s.setGlobal(p + q + QLatin1Char('_') + t.name, t.charge, charge,
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
            // The resistance of the first terminal to the others and the
            // grounds; the matrix's inverse.
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
                        s.setGlobal(matrixName(f.tag, QStringLiteral("R"), a, b), r(i, j), Dim::resistance(),
                                    tr("resistance matrix, %1 %2").arg(a, b));
                        if (a.size() == 1 && b.size() == 1) s.setGlobal(p + QStringLiteral("R") + a + b, r(i, j), Dim::resistance());
                    }
            }
        }
    }
    if (a_es) s.setGlobal(p + QStringLiteral("W"), 0.5 * a_energy, Dim::of(2, 1, -2), tr("electric energy"));
    if (a_ec) s.setGlobal(p + QStringLiteral("P"), a_energy, Dim::power(), tr("power dissipated"));
    if (a_heat) {
        double tmax = -INFINITY, tmin = INFINITY;
        const Mesh& mesh = s.mesh();
        for (int t = 0; t < f.space->elements(); ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            for (int i = 0; i < f.space->perElement(); ++i) {
                const double v = f.values[std::size_t(f.space->dofs(t)[i])];
                tmax = std::max(tmax, v);
                tmin = std::min(tmin, v);
            }
        }
        s.setGlobal(p + QStringLiteral("Tmax"), tmax, Dim::temperature(), tr("highest temperature"));
        s.setGlobal(p + QStringLiteral("Tmin"), tmin, Dim::temperature(), tr("lowest temperature"));
        s.setGlobal(p + QStringLiteral("P"), a_totalSource, Dim::power(), tr("heat made by the sources"));
        if (!std::isnan(a_sink) && a_totalSource != 0)
            s.setGlobal(p + QStringLiteral("Rth"), (tmax - a_sink) / a_totalSource, Dim::of(-2, -1, 3, 0, 1),
                        tr("thermal resistance: (Tmax - the coolest boundary temperature) / P"));
    }
}

} // namespace

std::unique_ptr<Problem> makeScalarProblem(Solution& solution, int field)
{
    return std::make_unique<ScalarProblem>(solution, field);
}

double scalarVacuumCapacitance(Solution& s, int fi, const QString& terminal, QString* error)
{
    // A field of its own, on the same space; the coefficients put back after.
    const Field saved = s.fields()[std::size_t(fi)];
    const Solution::Coefficients coefficients = s.coefficients()[std::size_t(fi)];
    ScalarProblem p(s, fi, true);
    QString why;
    double c = NaN;
    if (p.setup(&why)) c = p.selfMatrix(terminal, &why);
    s.coefficients()[std::size_t(fi)] = coefficients;
    s.fields()[std::size_t(fi)] = saved;
    if (std::isnan(c) && error) *error = why;
    return c;
}

} // namespace qucs_s::fem::detail
