/*
 * fem_pde.cpp - a PDE of the user's own: ea ∂²u/∂t² + da ∂u/∂t + ∇·Γ = F
 *               for one or several dependent variables (the general form;
 *               the coefficient form's Γ = -c∇u - αu + γ and
 *               F = f - β·∇u - a u built from it), Γ and F any expressions
 *               of x, y, t, the variables and their gradients, other
 *               physics' fields; on the mesh in 2D (about an axis too) or
 *               in 1D along a line across the geometry - an ODE in x - with
 *               Dirichlet, flux (Robin: -n·Γ = g - q u) and zero flux
 *               conditions; solved by Newton's method (the Jacobian of the
 *               expressions by differences, each variable and gradient
 *               nudged in turn) - a linear one by one solve, its matrix
 *               factored once while it stays -, in time (BDF; a second
 *               derivative through the variables' rates), or for its
 *               eigenvalues
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_edge.h"
#include "fem_line.h"
#include "fem_problem.h"
#include "fem_results.h"

#include <Eigen/SparseLU>

#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>

namespace qucs_s::fem::detail {

namespace {

/// The PDE's header kept with the field: kind (1 a line), the line (m),
/// its nodes' count, the components.
constexpr int Header = 7;

/// A domain's equations, compiled: by equation (variable), its flux Γ (x,
/// y), its source F, its damping and mass coefficients.
struct Equations {
    std::vector<Expression> gx, gy, f, da, ea;
    bool on = false;
};

/// A boundary's conditions: by equation, Dirichlet's value (invalid: free),
/// a flux's g and q (invalid: none).
struct Conditions {
    std::vector<Expression> r, g, q;
    bool dirichlet = false, flux = false;
};

/// Where a degree of freedom is: a triangle of the physics and its point there.
struct DofPlace {
    int t = -1;
    double xi = 0, eta = 0;
};

class PdeProblem : public Problem
{
public:
    PdeProblem(Solution& solution, int field) : Problem(solution, field) {}

    bool setup(QString* error) override;
    void initialize() override;
    bool solve(const SolveOptions& options, QString* error) override;
    void setGlobals() override;
    bool hasMass() const override { return a_mass; }
    int solveModes(const SolveOptions& options, int count, double guess, QString* error) override;
    void selectMode(int k) override;

private:
    /// The variables' values (by component and node) in the field: where
    /// each is in its values.
    std::size_t at(int c, int node) const
    {
        return a_line ? std::size_t(a_comps) * std::size_t(a_n2) + std::size_t(c) * std::size_t(a_nodes) + std::size_t(node)
                      : std::size_t(c) * std::size_t(a_n2) + std::size_t(node);
    }
    /// The Dirichlet values at the time solved; the equations numbered.
    void applyDirichlet();
    /// F(U) and its Jacobian (\a jac: none for the residual alone), \a mass
    /// and \a history the step in time's (0, none: at rest). \a eigen: the
    /// time terms left out, the eigenproblem's mass in \a massMatrix.
    void assemble(double mass, const std::vector<double>* history, Eigen::VectorXd& F, std::vector<Eigen::Triplet<double>>* jac,
                  std::vector<Eigen::Triplet<double>>* massMatrix = nullptr);
    /// The 2D part of a line's values: each degree of freedom's projection.
    void project();
    double scaleOf(int c) const;

    bool a_line = false;
    int a_axis = 0;
    int a_vars = 1, a_comps = 1;   // variables, and with their rates (a second derivative in time)
    int a_system = 1;              // the components solved for: all, or (an eigenproblem) the variables
    bool a_second = false, a_mass = false;
    int a_n2 = 0;                  // the 2D space's degrees of freedom
    int a_nodes = 0;               // the unknowns' nodes: the line's, or the 2D space's
    Line a_lineMesh;
    std::vector<Equations> a_eqs;          // by domain
    std::vector<Conditions> a_bc;          // by boundary
    std::vector<Expression> a_u0, a_ut0;   // by equation, by domain: [d * vars + k]
    std::vector<DofPlace> a_places;        // by node (2D: dof; 1D: a line node's place in the mesh)
    std::vector<DofPlace> a_linePoints;    // a line's points of integration: their places (by element, 4 each)
    std::vector<int> a_eq;                 // by component and node: the unknown, -1 fixed or none
    std::vector<char> a_fixed;             // by component and node: a Dirichlet value
    std::vector<char> a_onNode;            // by node: in the physics
    int a_neq = 0;
    double a_size = 1;                     // the model's size (m): the gradients' scale
    // Eigenvalues found.
    struct Eigen1 {
        std::complex<double> lambda;
        Eigen::VectorXd x;   // by unknown
    };
    std::vector<Eigen1> a_eigen;
    int a_selected = -1;
    int a_iterations = 0;
    // Linear: Γ, F and a flux's g affine in the unknowns (the variables,
    // their rates and gradients), da, ea and q free of them. Its matrix
    // varies where those parts read t or another physics' field.
    bool a_linear = false, a_matrixVaries = false;
    // The matrix factored: for which mass, unknowns and pattern (kept
    // across a step in time's solves, and a nonlinear one's analysis).
    std::unique_ptr<SparseLu<double>> a_lu;
    bool a_factored = false;
    int a_factorizations = 0;
    double a_factoredMass = -1;
    int a_factoredNeq = -1, a_factoredSystem = -1;
    std::vector<Eigen::Index> a_outer, a_inner;   // the analysed pattern
    /// \a J factored in a_lu (its pattern analysed again only when it is
    /// another); false and why on a singular one.
    bool factor(const Eigen::SparseMatrix<double>& J, QString* error);
};

double PdeProblem::scaleOf(int c) const
{
    const Field& f = a_s.fields()[std::size_t(a_fi)];
    double m = 0;
    for (int i = 0; i < a_nodes; ++i) m = std::max(m, std::abs(f.values[at(c, i)]));
    return m > 0 ? m : 1.0;
}

bool PdeProblem::setup(QString* error)
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
    QString why;
    const QVector<int> domains = resolveSelection(topo, model, Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(topo.domains.size(), 0);
    for (int d : domains) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    a_line = a_node->text(QStringLiteral("space")) == QLatin1String("1d");
    a_axis = a_node->text(QStringLiteral("axis")) == QLatin1String("y") ? 1 : 0;
    a_vars = int(pdeVariables(*a_node).size());
    a_second = pdeSecondOrder(*a_node);
    a_comps = int(f.components.size());
    a_system = a_comps;
    if (a_comps != a_vars * (a_second ? 2 : 1)) {
        *error = tr("%1: its variables changed since it was set up").arg(a_node->name());
        return false;
    }
    a_n2 = sp.size();
    {
        const QRectF b = mesh.bounds();
        a_size = std::max(1e-30, std::max(b.width(), b.height()) * mesh.unitScale);
    }
    // Each domain's equations: its last equation feature's, made general.
    const QStringList names = f.components.mid(0, a_vars);
    const QString p = f.tag + QLatin1Char('.');
    a_eqs.assign(topo.domains.size(), Equations());
    a_bc.assign(topo.boundaries.size(), Conditions());
    for (Conditions& c : a_bc) {
        c.r.resize(static_cast<std::size_t>(a_vars));
        c.g.resize(static_cast<std::size_t>(a_vars));
        c.q.resize(static_cast<std::size_t>(a_vars));
    }
    a_u0.assign(topo.domains.size() * std::size_t(a_vars), Expression());
    a_ut0.assign(topo.domains.size() * std::size_t(a_vars), Expression());
    // A property's text for equation k: its items split by ';' (one: all alike).
    auto item = [&](const Node& n, const QString& key, int k, const QString& fallback = QStringLiteral("0")) {
        const QString t = n.text(key);
        if (t.trimmed().isEmpty()) return fallback;
        const QStringList parts = t.split(QLatin1Char(';'));
        const QString v = (parts.size() == 1 ? parts.front() : k < parts.size() ? parts[k] : QString()).trimmed();
        return v.isEmpty() ? fallback : v;
    };
    auto pairItem = [&](const Node& n, const QString& key, int axis, int k) {
        const QStringList pr = n.pair(key);
        if (pr.size() < 2) return QStringLiteral("0");
        const QStringList parts = pr[axis].split(QLatin1Char(';'));
        const QString v = (parts.size() == 1 ? parts.front() : k < parts.size() ? parts[k] : QString()).trimmed();
        return v.isEmpty() ? QStringLiteral("0") : v;
    };
    std::vector<const Node*> eqOf(topo.domains.size(), nullptr), initOf(topo.domains.size(), nullptr);
    for (const Node& feature : a_node->children) {
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k) continue;
        const QVector<int> picked = resolveSelection(topo, model, k->selection, feature.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(feature.name(), why));
        const QString t = feature.type;
        if (t == QLatin1String("pde/coefficient") || t == QLatin1String("pde/general"))
            for (int d : picked) eqOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("pde/init"))
            for (int d : picked) initOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("pde/dirichlet") || t == QLatin1String("pde/flux")) {
            const bool dirichlet = t == QLatin1String("pde/dirichlet");
            for (int b : picked) {
                Conditions& c = a_bc[std::size_t(b)];
                for (int v = 0; v < a_vars; ++v) {
                    if (dirichlet) {
                        // An item left empty: that variable free here.
                        const QStringList parts = feature.text(QStringLiteral("r")).split(QLatin1Char(';'));
                        const QString r = (parts.size() == 1 ? parts.front() : v < parts.size() ? parts[v] : QString()).trimmed();
                        const QString constrained = feature.text(QStringLiteral("which")).trimmed();
                        if (r.isEmpty() || (!constrained.isEmpty() && !constrained.split(QRegularExpression(QStringLiteral("[,; ]+")),
                                                                                              Qt::SkipEmptyParts).contains(names[v])))
                            continue;
                        c.r[std::size_t(v)] = compile(r, feature, QStringLiteral("r"));
                        c.dirichlet = true;
                    } else {
                        // (Their units Γ's - as the equation's coefficients
                        // make it, and the flux's q per its variable's -:
                        // not checked against a fixed one.)
                        c.g[std::size_t(v)] = compile(item(feature, QStringLiteral("g"), v), feature, QStringLiteral("g"), true, false);
                        c.q[std::size_t(v)] = compile(item(feature, QStringLiteral("q"), v), feature, QStringLiteral("q"), true, false);
                        c.flux = true;
                    }
                }
            }
        }
    }
    a_mass = false;
    for (int d : domains) {
        const Node* e = eqOf[std::size_t(d)];
        const QString where = tr("domain %1").arg(d + 1);
        if (!e) {
            problem(tr("%1: %2 has no equation (a Coefficient Form or General Form PDE)").arg(a_node->name(), where));
            continue;
        }
        Equations& q = a_eqs[std::size_t(d)];
        q.on = true;
        for (int k = 0; k < a_vars; ++k) {
            const QString u = p + names[k], ux = p + names[k] + QLatin1Char('x'), uy = p + names[k] + QLatin1Char('y');
            QString gx, gy, src;
            if (e->type == QLatin1String("pde/general")) {
                gx = item(*e, QStringLiteral("Gx"), k);
                gy = item(*e, QStringLiteral("Gy"), k);
                src = item(*e, QStringLiteral("F"), k);
            } else {
                // Γ = -c ∇u - α u + γ, F = f - β·∇u - a u.
                const bool aniso = e->text(QStringLiteral("diffusion")) == QLatin1String("anisotropic");
                const QString cxx = aniso ? item(*e, QStringLiteral("cxx"), k, QStringLiteral("1")) : item(*e, QStringLiteral("c"), k, QStringLiteral("1"));
                const QString cyy = aniso ? item(*e, QStringLiteral("cyy"), k, QStringLiteral("1")) : cxx;
                const QString cxy = aniso ? item(*e, QStringLiteral("cxy"), k) : QStringLiteral("0");
                const QString cyx = aniso ? item(*e, QStringLiteral("cyx"), k) : QStringLiteral("0");
                gx = QStringLiteral("-(%1)*(%2) - (%3)*(%4) - (%5)*(%6) + (%7)")
                         .arg(cxx, ux, cxy, uy, pairItem(*e, QStringLiteral("alpha"), 0, k), u, pairItem(*e, QStringLiteral("gamma"), 0, k));
                gy = QStringLiteral("-(%1)*(%2) - (%3)*(%4) - (%5)*(%6) + (%7)")
                         .arg(cyx, ux, cyy, uy, pairItem(*e, QStringLiteral("alpha"), 1, k), u, pairItem(*e, QStringLiteral("gamma"), 1, k));
                src = QStringLiteral("(%1) - (%2)*(%3) - (%4)*(%5) - (%6)*(%7)")
                          .arg(item(*e, QStringLiteral("f"), k), pairItem(*e, QStringLiteral("beta"), 0, k), ux,
                               pairItem(*e, QStringLiteral("beta"), 1, k), uy, item(*e, QStringLiteral("a"), k), u);
            }
            q.gx.push_back(compile(gx, *e, QStringLiteral("Gx")));
            q.gy.push_back(compile(gy, *e, QStringLiteral("Gy")));
            q.f.push_back(compile(src, *e, QStringLiteral("F")));
            q.da.push_back(compile(item(*e, QStringLiteral("da"), k), *e, QStringLiteral("da")));
            q.ea.push_back(compile(item(*e, QStringLiteral("ea"), k), *e, QStringLiteral("ea")));
            for (const Expression* x : {&q.da.back(), &q.ea.back()})
                if (x->isValid() && !(x->isConstant() && x->constant() == 0)) a_mass = true;
        }
        if (const Node* in = initOf[std::size_t(d)])
            for (int k = 0; k < a_vars; ++k) {
                a_u0[std::size_t(d) * std::size_t(a_vars) + std::size_t(k)] = compile(item(*in, QStringLiteral("u0"), k), *in, QStringLiteral("u0"));
                a_ut0[std::size_t(d) * std::size_t(a_vars) + std::size_t(k)] = compile(item(*in, QStringLiteral("ut0"), k), *in, QStringLiteral("ut0"));
            }
    }
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    // Linear or not: each expression's degree in the unknowns.
    {
        const std::vector<Solution::VariableDef>& defs = a_s.definitions();
        const int self = a_fi;
        auto kind = [&defs, self](int slot) {
            using K = Expression::SlotKind;
            if (slot < 0 || slot >= int(defs.size())) return K::Varying;
            const Solution::VariableDef& d = defs[std::size_t(slot)];
            switch (d.kind) {
            case Solution::VariableDef::X:
            case Solution::VariableDef::Y:
            case Solution::VariableDef::DomainNumber:
            case Solution::VariableDef::Size:
            case Solution::VariableDef::Frequency: return K::Static;
            case Solution::VariableDef::Time: return K::Varying;
            case Solution::VariableDef::Value:
            case Solution::VariableDef::GradX:
            case Solution::VariableDef::GradY: return d.field == self ? K::Unknown : K::Varying;
            case Solution::VariableDef::Derived:
                // (Its own along a line: the line's values and slopes, as
                // linear in its unknowns as a value is.)
                if (d.field == self) return K::Unknown;
                return d.field < 0 ? K::Static : K::Varying;
            default:
                // (|∇u|, a coefficient worked out of a field: not a sum of the unknowns.)
                return d.field == self ? K::Opaque : K::Varying;
            }
        };
        a_linear = true;
        a_matrixVaries = false;
        // \a affine: its degree may be 1 (its coefficient then the matrix's),
        // else 0 (it multiplies an unknown: the matrix's whole).
        auto take = [&](const Expression& e, bool affine) {
            if (!e.isValid()) return;
            const Expression::Linearity l = e.linearity(kind);
            if (l.degree < 0 || l.degree > (affine ? 1 : 0)) a_linear = false;
            if (affine ? l.varyingCoefficient : l.varying) a_matrixVaries = true;
        };
        for (const Equations& q : a_eqs) {
            if (!q.on) continue;
            for (const auto* list : {&q.gx, &q.gy, &q.f})
                for (const Expression& e : *list) take(e, true);
            for (const auto* list : {&q.da, &q.ea})
                for (const Expression& e : *list) take(e, false);
        }
        for (const Conditions& c : a_bc) {
            for (const Expression& e : c.g) take(e, true);
            for (const Expression& e : c.q) take(e, false);
            for (const Expression& e : c.r) {
                // (A Dirichlet value: of the unknowns, not linear; what varies, the values alone.)
                if (e.isValid() && e.linearity(kind).degree != 0) a_linear = false;
            }
        }
    }
    a_lu.reset();
    a_factored = false;
    a_factorizations = 0;
    a_s.log << (a_linear ? tr("%1: linear in its unknowns - solved at once, no Newton's iterations; its matrix factored %2")
                               .arg(a_node->name(), a_matrixVaries ? tr("at each solve (what multiplies them varies in time)")
                                                                   : tr("once while a time step stays"))
                         : tr("%1: nonlinear in its unknowns - Newton's method").arg(a_node->name()));
    // The nodes: the line's, or the 2D space's degrees of freedom; where each is.
    if (a_line) {
        QString lineWhy;
        int count = a_node->integer(QStringLiteral("elements"));
        if (!detail::buildLine(a_s, f.domains, a_axis, a_node->text(QStringLiteral("at")), 0, std::max(1, count), &a_lineMesh, &lineWhy)) {
            *error = tr("%1: %2").arg(a_node->name(), lineWhy);
            return false;
        }
        a_nodes = int(a_lineMesh.z.size());
        a_onNode.assign(std::size_t(a_nodes), 0);
        for (int e = 0; e < a_lineMesh.elements(); ++e)
            if (a_lineMesh.domain[std::size_t(e)] >= 0)
                for (int k = 0; k < 3; ++k) a_onNode[std::size_t(2 * e + k)] = 1;
        // Each node's place in the mesh (nudged across the line off a mesh edge).
        const Locator locator(mesh);
        const QPointF u = (a_lineMesh.E - a_lineMesh.S) / a_lineMesh.L, across(-u.y(), u.x());
        a_places.assign(std::size_t(a_nodes), DofPlace());
        for (int i = 0; i < a_nodes; ++i) {
            // (At a layer's end, a little into the element it is in.)
            const double s = a_lineMesh.z[std::size_t(i)];
            for (double nudge : {0.0, 1e-7, -1e-7}) {
                const QPointF pt = a_lineMesh.point(s) + across * (nudge * a_lineMesh.L);
                int t = -1;
                double xi = 0, eta = 0;
                if (locator.locate(pt / mesh.unitScale, &t, &xi, &eta)) {
                    const int d = mesh.domain[std::size_t(t)];
                    if (d >= 0 && f.domains[std::size_t(d)]) {
                        a_places[std::size_t(i)] = {t, xi, eta};
                        break;
                    }
                    if (a_places[std::size_t(i)].t < 0) a_places[std::size_t(i)] = {t, xi, eta};
                }
            }
        }
        // Its points of integration's places, once.
        a_linePoints.assign(std::size_t(4 * a_lineMesh.elements()), DofPlace());
        const auto& rule = lineRule(4);
        for (int e = 0; e < a_lineMesh.elements(); ++e) {
            if (a_lineMesh.domain[std::size_t(e)] < 0) continue;
            const double z0 = a_lineMesh.z[std::size_t(2 * e)], z2 = a_lineMesh.z[std::size_t(2 * e + 2)];
            for (std::size_t q = 0; q < rule.size() && q < 4; ++q) {
                const QPointF x = a_lineMesh.point(z0 + rule[q][0] * (z2 - z0));
                for (double nudge : {0.0, 1e-7, -1e-7}) {
                    int t = -1;
                    double xi = 0, eta = 0;
                    if (locator.locate((x + across * (nudge * a_lineMesh.L)) / mesh.unitScale, &t, &xi, &eta)) {
                        a_linePoints[std::size_t(4 * e) + q] = {t, xi, eta};
                        break;
                    }
                }
            }
        }
        // The line kept with the field, for its variables.
        auto fixed = std::make_shared<std::vector<double>>();
        fixed->assign(std::size_t(Header), 0);
        (*fixed)[0] = 1;
        (*fixed)[1] = a_lineMesh.S.x();
        (*fixed)[2] = a_lineMesh.S.y();
        (*fixed)[3] = a_lineMesh.E.x();
        (*fixed)[4] = a_lineMesh.E.y();
        (*fixed)[5] = a_nodes;
        (*fixed)[6] = a_comps;
        fixed->insert(fixed->end(), a_lineMesh.z.begin(), a_lineMesh.z.end());
        f.fixed = fixed;
    } else {
        a_nodes = a_n2;
        a_onNode.assign(std::size_t(a_nodes), 0);
        a_places.assign(std::size_t(a_nodes), DofPlace());
        static const double ref[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0.5, 0}, {0.5, 0.5}, {0, 0.5}};
        for (int t = 0; t < sp.elements(); ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            for (int i = 0; i < sp.perElement(); ++i) {
                const int dof = sp.dofs(t)[i];
                a_onNode[std::size_t(dof)] = 1;
                if (a_places[std::size_t(dof)].t < 0) a_places[std::size_t(dof)] = {t, ref[i][0], ref[i][1]};
            }
        }
        f.fixed.reset();
    }
    const std::size_t size = std::size_t(a_comps) * std::size_t(a_n2) + (a_line ? std::size_t(a_comps) * std::size_t(a_nodes) : 0);
    if (f.values.size() != size) f.values.assign(size, 0.0);
    return true;
}

void PdeProblem::initialize()
{
    Field& f = field();
    const std::size_t size = std::size_t(a_comps) * std::size_t(a_n2) + (a_line ? std::size_t(a_comps) * std::size_t(a_nodes) : 0);
    f.values.assign(size, 0.0);
    f.extra.clear();
    PointEvaluator ev(a_s);
    const Mesh& mesh = a_s.mesh();
    for (int i = 0; i < a_nodes; ++i) {
        const DofPlace& pl = a_places[std::size_t(i)];
        if (pl.t < 0 || !a_onNode[std::size_t(i)]) continue;
        const int d = mesh.domain[std::size_t(pl.t)];
        if (d < 0) continue;
        ev.moveTo(pl.t, pl.xi, pl.eta);
        for (int k = 0; k < a_vars; ++k) {
            const std::size_t at0 = std::size_t(d) * std::size_t(a_vars) + std::size_t(k);
            if (at0 < a_u0.size() && a_u0[at0].isValid()) f.values[at(k, i)] = ev.eval(a_u0[at0]);
            if (a_second && at0 < a_ut0.size() && a_ut0[at0].isValid()) f.values[at(a_vars + k, i)] = ev.eval(a_ut0[at0]);
        }
    }
    for (double& v : f.values)
        if (!std::isfinite(v)) v = 0;
    project();
}

void PdeProblem::project()
{
    if (!a_line) return;
    Field& f = field();
    const Space& sp = *f.space;
    for (int dof = 0; dof < a_n2; ++dof) {
        const double s = a_lineMesh.along(sp.point(dof));
        if (s < 0 || s > a_lineMesh.L) continue;
        double xi = 0;
        const int e = a_lineMesh.element(s, &xi);
        double N[3], dN[3];
        lineShape(xi, N, dN);
        for (int c = 0; c < a_comps; ++c) {
            double v = 0;
            for (int k = 0; k < 3; ++k) v += N[k] * f.values[at(c, 2 * e + k)];
            f.values[std::size_t(c) * std::size_t(a_n2) + std::size_t(dof)] = v;
        }
    }
}

void PdeProblem::applyDirichlet()
{
    Field& f = field();
    const Mesh& mesh = a_s.mesh();
    const Space& sp = *f.space;
    a_fixed.assign(std::size_t(a_comps) * std::size_t(a_nodes), 0);
    PointEvaluator ev(a_s);
    auto fix = [&](int node, int b) {
        const Conditions& c = a_bc[std::size_t(b)];
        if (!c.dirichlet) return;
        const DofPlace& pl = a_places[std::size_t(node)];
        if (pl.t < 0) return;
        ev.moveTo(pl.t, pl.xi, pl.eta);
        for (int k = 0; k < a_vars; ++k) {
            if (!c.r[std::size_t(k)].isValid()) continue;
            const double v = ev.eval(c.r[std::size_t(k)]);
            f.values[at(k, node)] = std::isfinite(v) ? v : 0.0;
            a_fixed[std::size_t(k) * std::size_t(a_nodes) + std::size_t(node)] = 1;
        }
    };
    if (a_line) {
        for (int i = 0; i < a_nodes; ++i)
            for (int b : a_lineMesh.boundaries[std::size_t(i)]) fix(i, b);
    } else {
        for (std::size_t e = 0; e < mesh.edges.size(); ++e) {
            const Mesh::Edge& me = mesh.edges[e];
            if (me.boundary < 0 || !a_bc[std::size_t(me.boundary)].dirichlet) continue;
            for (int dof : sp.edgeDofs(int(e)))
                if (dof >= 0 && a_onNode[std::size_t(dof)]) fix(dof, me.boundary);
        }
    }
    // The unknowns: each component at each node of the physics not fixed.
    a_eq.assign(std::size_t(a_comps) * std::size_t(a_nodes), -1);
    a_neq = 0;
    for (int c = 0; c < a_system; ++c)
        for (int i = 0; i < a_nodes; ++i) {
            const std::size_t k = std::size_t(c) * std::size_t(a_nodes) + std::size_t(i);
            if (a_onNode[std::size_t(i)] && !(c < a_vars && a_fixed[k])) a_eq[k] = a_neq++;
        }
}

void PdeProblem::assemble(double mass, const std::vector<double>* history, Eigen::VectorXd& F, std::vector<Eigen::Triplet<double>>* jac,
                          std::vector<Eigen::Triplet<double>>* massMatrix)
{
    const Field& fld = field();
    const Mesh& mesh = a_s.mesh();
    const Space& sp = *fld.space;
    const bool axi = a_s.axisymmetric();
    F = Eigen::VectorXd::Zero(a_neq);
    if (jac) jac->clear();
    if (massMatrix) massMatrix->clear();
    std::vector<double> scale(static_cast<std::size_t>(a_comps));
    for (int c = 0; c < a_comps; ++c) scale[std::size_t(c)] = scaleOf(c);
    const bool second = a_second && a_system == a_comps;
    const int which = a_line ? 2 : 3;   // a value and its derivatives: along a line, or x and y
    std::mutex merge;
    // An element's part: its nodes, shape functions and derivatives at a
    // point of integration, its weight, the evaluator there.
    struct Point {
        int n = 0;
        int node[6];
        double N[6], dx[6], dy[6];   // (a line's: dx along it)
        double w = 0;
        int d = -1;
    };
    // Which of a domain's expressions (each equation's gx, gy and f) each
    // nudge of the Jacobian's can change: those that read the component
    // nudged - its value, or that derivative -, or a derived variable or a
    // coefficient (what those read is not known here). The others are not
    // evaluated again: the same numbers. (Every one was, at every nudge: a
    // 681 054-unknown PDE's assembly 2.3 s, most of its solve once its
    // factorization took 0.2.)
    const int nudges = a_system * which;
    std::vector<std::vector<char>> moves(a_eqs.size());
    if (jac) {
        const auto& defs = a_s.definitions();
        using K = Solution::VariableDef;
        auto depends = [&defs, this](const Expression& e, int c, int wch) {
            if (!e.isValid() || e.isConstant()) return false;
            for (int slot : e.variables()) {
                const K& d = defs[std::size_t(slot)];
                if (d.kind == K::Derived || d.kind == K::Coefficient) return true;
                if (d.field != a_fi || d.component != c) continue;
                if (d.kind == K::Value && wch == 0) return true;
                if ((d.kind == K::GradX || d.kind == K::GradY || d.kind == K::GradNorm) && wch != 0
                    && (a_line || d.kind == K::GradNorm || (d.kind == K::GradX) == (wch == 1)))
                    return true;
            }
            return false;
        };
        for (std::size_t dm = 0; dm < a_eqs.size(); ++dm) {
            const Equations& q = a_eqs[dm];
            if (!q.on) continue;
            std::vector<char>& m = moves[dm];
            m.assign(std::size_t(nudges) * std::size_t(3 * a_vars), 0);
            for (int c = 0; c < a_system; ++c)
                for (int w = 0; w < which; ++w)
                    for (int k = 0; k < a_vars; ++k) {
                        const std::size_t at = std::size_t(c * which + w) * std::size_t(3 * a_vars) + std::size_t(3 * k);
                        m[at] = depends(q.gx[std::size_t(k)], c, w);
                        m[at + 1] = depends(q.gy[std::size_t(k)], c, w);
                        m[at + 2] = depends(q.f[std::size_t(k)], c, w);
                    }
        }
    }
    auto contribute = [&](PointEvaluator& ev, const Point& pt, Eigen::VectorXd& Fl, std::vector<Eigen::Triplet<double>>& Jl,
                          std::vector<Eigen::Triplet<double>>& Ml) {
        const Equations& q = a_eqs[std::size_t(pt.d)];
        const QPointF dir = a_line ? (a_lineMesh.E - a_lineMesh.S) / a_lineMesh.L : QPointF(1, 0);
        // Each equation's flux (along a line: its component along it) and source.
        auto values = [&](std::vector<double>& gx, std::vector<double>& gy, std::vector<double>& src) {
            for (int k = 0; k < a_vars; ++k) {
                const double vx = ev.eval(q.gx[std::size_t(k)]), vy = ev.eval(q.gy[std::size_t(k)]);
                gx[std::size_t(k)] = a_line ? vx * dir.x() + vy * dir.y() : vx;
                gy[std::size_t(k)] = a_line ? 0 : vy;
                src[std::size_t(k)] = ev.eval(q.f[std::size_t(k)]);
            }
        };
        std::vector<double> gx(static_cast<std::size_t>(a_vars)), gy(static_cast<std::size_t>(a_vars)), src(static_cast<std::size_t>(a_vars));
        values(gx, gy, src);
        // The variables and rates here, and the time terms.
        auto valueAt = [&](int c) {
            double v = 0;
            for (int i = 0; i < pt.n; ++i) v += pt.N[i] * fld.values[at(c, pt.node[i])];
            return v;
        };
        auto historyAt = [&](int c) {
            double v = 0;
            if (history)
                for (int i = 0; i < pt.n; ++i) v += pt.N[i] * (*history)[at(c, pt.node[i])];
            return v;
        };
        for (int k = 0; k < a_vars; ++k) {
            const double da = ev.eval(q.da[std::size_t(k)]), ea = ev.eval(q.ea[std::size_t(k)]);
            const double u = valueAt(k);
            double ut = 0, wt = 0;
            if (second) {
                const double w = valueAt(a_vars + k);
                ut = w;
                wt = mass > 0 ? mass * w - historyAt(a_vars + k) : 0;
            } else ut = mass > 0 ? mass * u - historyAt(k) : 0;
            const double rhs = src[std::size_t(k)] - (std::isfinite(da) ? da : 0) * ut - (std::isfinite(ea) ? ea : 0) * wt;
            for (int i = 0; i < pt.n; ++i) {
                const int r = a_eq[std::size_t(k) * std::size_t(a_nodes) + std::size_t(pt.node[i])];
                if (r < 0) continue;
                Fl(r) += pt.w * (gx[std::size_t(k)] * pt.dx[i] + gy[std::size_t(k)] * pt.dy[i] + rhs * pt.N[i]);
                if (!jac && !massMatrix) continue;
                for (int m = 0; m < pt.n; ++m) {
                    const double nn = pt.w * pt.N[i] * pt.N[m];
                    if (massMatrix) {
                        // The eigenproblem's mass: da's, else ea's, else 1.
                        const double em = std::isfinite(da) && da != 0 ? da : std::isfinite(ea) && ea != 0 ? ea : 1.0;
                        const int col = a_eq[std::size_t(k) * std::size_t(a_nodes) + std::size_t(pt.node[m])];
                        if (col >= 0) Ml.emplace_back(r, col, em * nn);
                    }
                    if (!jac || mass <= 0) continue;
                    if (second) {
                        const int cw = a_eq[std::size_t(a_vars + k) * std::size_t(a_nodes) + std::size_t(pt.node[m])];
                        if (cw >= 0) Jl.emplace_back(r, cw, -(da + ea * mass) * nn);
                    } else {
                        const int cu = a_eq[std::size_t(k) * std::size_t(a_nodes) + std::size_t(pt.node[m])];
                        if (cu >= 0) Jl.emplace_back(r, cu, -da * mass * nn);
                    }
                }
            }
            // A rate's own equation: w - ∂u/∂t = 0.
            if (second)
                for (int i = 0; i < pt.n; ++i) {
                    const int r = a_eq[std::size_t(a_vars + k) * std::size_t(a_nodes) + std::size_t(pt.node[i])];
                    if (r < 0) continue;
                    const double w = valueAt(a_vars + k);
                    const double rate = mass > 0 ? mass * u - historyAt(k) : 0;
                    Fl(r) += pt.w * (w - rate) * pt.N[i];
                    if (!jac) continue;
                    for (int m = 0; m < pt.n; ++m) {
                        const double nn = pt.w * pt.N[i] * pt.N[m];
                        const int cw = a_eq[std::size_t(a_vars + k) * std::size_t(a_nodes) + std::size_t(pt.node[m])];
                        if (cw >= 0) Jl.emplace_back(r, cw, nn);
                        const int cu = a_eq[std::size_t(k) * std::size_t(a_nodes) + std::size_t(pt.node[m])];
                        if (cu >= 0 && mass > 0) Jl.emplace_back(r, cu, -mass * nn);
                    }
                }
        }
        if (!jac) return;
        // The Jacobian of Γ and F: each component's value and derivatives
        // nudged in turn.
        std::vector<double> gx2(static_cast<std::size_t>(a_vars)), gy2(static_cast<std::size_t>(a_vars)), src2(static_cast<std::size_t>(a_vars));
        for (int c = 0; c < a_system; ++c)
            for (int wch = 0; wch < which; ++wch) {
                double base = 0;
                for (int i = 0; i < pt.n; ++i) {
                    const double b = wch == 0 ? pt.N[i] : wch == 1 ? pt.dx[i] : pt.dy[i];
                    base += b * fld.values[at(c, pt.node[i])];
                }
                const double sc = wch == 0 ? scale[std::size_t(c)] : scale[std::size_t(c)] / a_size;
                // (Affine in it, any nudge gives its slope exactly: a linear
                // problem's whole scale, its rounding 1e-16 - not 1e-9, a
                // small nudge's - and one solve exact.)
                const double h = (a_linear ? 1.0 : 1e-7) * std::max(std::abs(base), sc);
                const std::vector<char>& mv = moves[std::size_t(pt.d)];
                const char* move = mv.empty() ? nullptr : &mv[std::size_t(c * which + wch) * std::size_t(3 * a_vars)];
                ev.shift(a_fi, c, wch, h);
                for (int k = 0; k < a_vars; ++k) {
                    if (!move || move[3 * k] || move[3 * k + 1]) {
                        const double vx = ev.eval(q.gx[std::size_t(k)]), vy = ev.eval(q.gy[std::size_t(k)]);
                        gx2[std::size_t(k)] = a_line ? vx * dir.x() + vy * dir.y() : vx;
                        gy2[std::size_t(k)] = a_line ? 0 : vy;
                    } else {
                        gx2[std::size_t(k)] = gx[std::size_t(k)];
                        gy2[std::size_t(k)] = gy[std::size_t(k)];
                    }
                    src2[std::size_t(k)] = !move || move[3 * k + 2] ? ev.eval(q.f[std::size_t(k)]) : src[std::size_t(k)];
                }
                ev.shift(a_fi, c, wch, 0);
                for (int k = 0; k < a_vars; ++k) {
                    const double dgx = (gx2[std::size_t(k)] - gx[std::size_t(k)]) / h, dgy = (gy2[std::size_t(k)] - gy[std::size_t(k)]) / h;
                    const double df = (src2[std::size_t(k)] - src[std::size_t(k)]) / h;
                    if (dgx == 0 && dgy == 0 && df == 0) continue;
                    for (int i = 0; i < pt.n; ++i) {
                        const int r = a_eq[std::size_t(k) * std::size_t(a_nodes) + std::size_t(pt.node[i])];
                        if (r < 0) continue;
                        const double test = dgx * pt.dx[i] + dgy * pt.dy[i] + df * pt.N[i];
                        for (int m = 0; m < pt.n; ++m) {
                            const int col = a_eq[std::size_t(c) * std::size_t(a_nodes) + std::size_t(pt.node[m])];
                            if (col < 0) continue;
                            const double b = wch == 0 ? pt.N[m] : wch == 1 ? pt.dx[m] : pt.dy[m];
                            Jl.emplace_back(r, col, pt.w * test * b);
                        }
                    }
                }
            }
    };
    if (a_line) {
        PointEvaluator ev(a_s);
        std::vector<Eigen::Triplet<double>> Jl, Ml;
        const auto& rule = lineRule(4);
        for (int e = 0; e < a_lineMesh.elements(); ++e) {
            const int d = a_lineMesh.domain[std::size_t(e)];
            if (d < 0 || !a_eqs[std::size_t(d)].on) continue;
            const double z0 = a_lineMesh.z[std::size_t(2 * e)], z2 = a_lineMesh.z[std::size_t(2 * e + 2)], h = z2 - z0;
            for (std::size_t qi = 0; qi < rule.size() && qi < 4; ++qi) {
                const auto& qp = rule[qi];
                Point pt;
                pt.n = 3;
                pt.d = d;
                double dN[3];
                lineShape(qp[0], pt.N, dN);
                for (int i = 0; i < 3; ++i) {
                    pt.node[i] = 2 * e + i;
                    pt.dx[i] = dN[i] / h;
                    pt.dy[i] = 0;
                }
                const QPointF x = a_lineMesh.point(z0 + qp[0] * h);
                pt.w = qp[1] * h * (axi ? 2 * M_PI * x.x() : 1.0);
                const DofPlace& pl = a_linePoints[std::size_t(4 * e) + qi];
                if (pl.t < 0) continue;
                ev.moveTo(pl.t, pl.xi, pl.eta);
                contribute(ev, pt, F, Jl, Ml);
            }
        }
        if (jac) jac->insert(jac->end(), Jl.begin(), Jl.end());
        if (massMatrix) massMatrix->insert(massMatrix->end(), Ml.begin(), Ml.end());
    } else {
        const TriangleRule& rule = triangleRule(sp.order() == 1 ? 3 : 5);
        parallelFor(sp.elements(), [&](int begin, int end) {
            PointEvaluator ev(a_s);
            Eigen::VectorXd Fl = Eigen::VectorXd::Zero(a_neq);
            std::vector<Eigen::Triplet<double>> Jl, Ml, Je;
            // An element's entries summed before they are kept: a few
            // rows and columns (its components at its nodes), each entry
            // once - not once a point of integration and a nudge.
            std::vector<int> rows, cols;
            std::vector<double> dense;
            std::vector<char> touched;
            auto keep = [&] {
                rows.clear();
                cols.clear();
                auto place = [](std::vector<int>& list, int v) {
                    for (std::size_t i = 0; i < list.size(); ++i)
                        if (list[i] == v) return int(i);
                    list.push_back(v);
                    return int(list.size()) - 1;
                };
                for (const auto& e : Je) {
                    place(rows, e.row());
                    place(cols, e.col());
                }
                dense.assign(rows.size() * cols.size(), 0.0);
                touched.assign(rows.size() * cols.size(), 0);
                for (const auto& e : Je) {
                    const std::size_t k = std::size_t(place(rows, e.row())) * cols.size() + std::size_t(place(cols, e.col()));
                    dense[k] += e.value();
                    touched[k] = 1;
                }
                for (std::size_t i = 0; i < rows.size(); ++i)
                    for (std::size_t j = 0; j < cols.size(); ++j)
                        if (touched[i * cols.size() + j]) Jl.emplace_back(rows[i], cols[j], dense[i * cols.size() + j]);
                Je.clear();
            };
            for (int t = begin; t < end; ++t) {
                const int d = mesh.domain[std::size_t(t)];
                if (d < 0 || !fld.domains[std::size_t(d)] || !a_eqs[std::size_t(d)].on) continue;
                for (const auto& qp : rule.points) {
                    const Space::Map m = sp.map(t, qp[0], qp[1]);
                    Shape sh;
                    shapeAt(sp.order(), qp[0], qp[1], sh);
                    Point pt;
                    pt.n = sh.count;
                    pt.d = d;
                    pt.w = qp[2] * std::abs(m.det) * (axi ? 2 * M_PI * m.x.x() : 1.0);
                    for (int i = 0; i < sh.count; ++i) {
                        pt.node[i] = sp.dofs(t)[i];
                        pt.N[i] = sh.n[i];
                        Space::gradient(m, sh.dxi[i], sh.deta[i], pt.dx[i], pt.dy[i]);
                    }
                    ev.moveTo(t, qp[0], qp[1]);
                    contribute(ev, pt, Fl, jac ? Je : Jl, Ml);
                }
                if (jac) keep();
            }
            std::lock_guard<std::mutex> lock(merge);
            F += Fl;
            if (jac) jac->insert(jac->end(), Jl.begin(), Jl.end());
            if (massMatrix) massMatrix->insert(massMatrix->end(), Ml.begin(), Ml.end());
        });
    }
    // Flux conditions: ∫ (g - q u) φ on their boundaries (a line's: at its nodes there).
    PointEvaluator ev(a_s);
    auto natural = [&](const Conditions& c, double weight, const int* nodes, const double* N, int n) {
        std::vector<double> g(std::size_t(a_vars), 0), qv(std::size_t(a_vars), 0), u(std::size_t(a_vars), 0);
        auto evaluate = [&](std::vector<double>& gg, std::vector<double>& qq) {
            for (int k = 0; k < a_vars; ++k) {
                gg[std::size_t(k)] = c.g[std::size_t(k)].isValid() ? ev.eval(c.g[std::size_t(k)]) : 0;
                qq[std::size_t(k)] = c.q[std::size_t(k)].isValid() ? ev.eval(c.q[std::size_t(k)]) : 0;
            }
        };
        evaluate(g, qv);
        for (int k = 0; k < a_vars; ++k)
            for (int i = 0; i < n; ++i) u[std::size_t(k)] += N[i] * fld.values[at(k, nodes[i])];
        for (int k = 0; k < a_vars; ++k) {
            if (!c.g[std::size_t(k)].isValid() && !c.q[std::size_t(k)].isValid()) continue;
            const double term = (std::isfinite(g[std::size_t(k)]) ? g[std::size_t(k)] : 0) - (std::isfinite(qv[std::size_t(k)]) ? qv[std::size_t(k)] : 0) * u[std::size_t(k)];
            for (int i = 0; i < n; ++i) {
                const int r = a_eq[std::size_t(k) * std::size_t(a_nodes) + std::size_t(nodes[i])];
                if (r < 0) continue;
                F(r) += weight * term * N[i];
                if (!jac) continue;
                for (int m = 0; m < n; ++m) {
                    const int col = a_eq[std::size_t(k) * std::size_t(a_nodes) + std::size_t(nodes[m])];
                    if (col >= 0) jac->emplace_back(r, col, -weight * qv[std::size_t(k)] * N[i] * N[m]);
                }
            }
        }
        if (!jac) return;
        // g and q reading the components solved - the variables and (in
        // time) their rates, an absorbing boundary's g = -ut/c: their
        // derivatives.
        std::vector<double> g2(static_cast<std::size_t>(a_vars)), q2(static_cast<std::size_t>(a_vars));
        for (int cc = 0; cc < a_system; ++cc) {
            double base = 0;
            for (int i = 0; i < n; ++i) base += N[i] * fld.values[at(cc, nodes[i])];
            const double h = (a_linear ? 1.0 : 1e-7) * std::max(std::abs(base), scale[std::size_t(cc)]);
            ev.shift(a_fi, cc, 0, h);
            evaluate(g2, q2);
            ev.shift(a_fi, cc, 0, 0);
            for (int k = 0; k < a_vars; ++k) {
                if (!c.g[std::size_t(k)].isValid() && !c.q[std::size_t(k)].isValid()) continue;
                const double dt = ((g2[std::size_t(k)] - g[std::size_t(k)]) - (q2[std::size_t(k)] - qv[std::size_t(k)]) * u[std::size_t(k)]) / h;
                if (dt == 0 || !std::isfinite(dt)) continue;
                for (int i = 0; i < n; ++i) {
                    const int r = a_eq[std::size_t(k) * std::size_t(a_nodes) + std::size_t(nodes[i])];
                    if (r < 0) continue;
                    for (int m = 0; m < n; ++m) {
                        const int col = a_eq[std::size_t(cc) * std::size_t(a_nodes) + std::size_t(nodes[m])];
                        if (col >= 0) jac->emplace_back(r, col, weight * dt * N[i] * N[m]);
                    }
                }
            }
        }
    };
    if (a_line) {
        for (int i = 0; i < a_nodes; ++i)
            for (int b : a_lineMesh.boundaries[std::size_t(i)]) {
                const Conditions& c = a_bc[std::size_t(b)];
                if (!c.flux || a_places[std::size_t(i)].t < 0 || !a_onNode[std::size_t(i)]) continue;
                const DofPlace& pl = a_places[std::size_t(i)];
                ev.moveTo(pl.t, pl.xi, pl.eta);
                const double one = 1;
                const QPointF x = a_lineMesh.point(a_lineMesh.z[std::size_t(i)]);
                natural(c, axi ? 2 * M_PI * x.x() : 1.0, &i, &one, 1);
            }
    } else {
        for (std::size_t e = 0; e < mesh.edges.size(); ++e) {
            const Mesh::Edge& me = mesh.edges[e];
            if (me.boundary < 0 || !a_bc[std::size_t(me.boundary)].flux) continue;
            int tri = -1;
            for (int side : {me.left, me.right})
                if (side >= 0 && mesh.domain[std::size_t(side)] >= 0 && fld.domains[std::size_t(mesh.domain[std::size_t(side)])]) {
                    tri = side;
                    break;
                }
            if (tri < 0) continue;
            const std::array<int, 3>& tv = mesh.triangles[std::size_t(tri)];
            const int k = Space::localEdge(tv, me.a, me.b);
            const bool same = tv[std::size_t(k)] == me.a;
            const std::array<int, 3> dofs = sp.edgeDofs(int(e));
            const int n = sp.order() == 1 ? 2 : 3;
            for (const auto& qp : lineRule(sp.order() + 2)) {
                double jacobian = 0;
                const QPointF x = sp.edgePoint(int(e), qp[0], &jacobian);
                const double s = same ? qp[0] : 1 - qp[0];
                const double xi = k == 0 ? s : k == 1 ? 1 - s : 0, eta = k == 0 ? 0 : k == 1 ? s : 1 - s;
                ev.moveTo(tri, xi, eta);
                const double t = qp[0];
                const double N[3] = {n == 2 ? 1 - t : (1 - t) * (1 - 2 * t), n == 2 ? t : t * (2 * t - 1), 4 * t * (1 - t)};
                natural(a_bc[std::size_t(me.boundary)], qp[1] * jacobian * (axi ? 2 * M_PI * x.x() : 1.0), dofs.data(), N, n);
            }
        }
    }
}

bool PdeProblem::factor(const Eigen::SparseMatrix<double>& J, QString* error)
{
    // (Its pattern - each column's rows - as the last one's: its analysis kept.)
    const Eigen::Index n = J.outerSize();
    const bool same = a_lu && Eigen::Index(a_outer.size()) == n + 1 && std::equal(a_outer.begin(), a_outer.end(), J.outerIndexPtr())
                      && Eigen::Index(a_inner.size()) == J.nonZeros() && std::equal(a_inner.begin(), a_inner.end(), J.innerIndexPtr());
    if (!same) {
        a_lu = std::make_unique<SparseLu<double>>();
        a_lu->analyzePattern(J);
        a_outer.assign(J.outerIndexPtr(), J.outerIndexPtr() + n + 1);
        a_inner.assign(J.innerIndexPtr(), J.innerIndexPtr() + J.nonZeros());
    }
    a_lu->factorize(J);
    ++a_factorizations;
    if (a_lu->info() != Eigen::Success) {
        *error = tr("%1: its Jacobian is singular (a variable fixed nowhere? a coefficient 0?): %2")
                     .arg(a_node->name(), QString::fromStdString(a_lu->lastErrorMessage()));
        a_factored = false;
        return false;
    }
    return true;
}

bool PdeProblem::solve(const SolveOptions& o, QString* error)
{
    Field& f = field();
    a_system = a_comps;
    applyDirichlet();
    if (a_neq == 0) {
        f.solved = true;
        project();
        return true;
    }
    // Newton's method: J δ = -F, its step halved while F grows.
    std::vector<int> unknown(static_cast<std::size_t>(a_neq));
    for (int c = 0; c < a_system; ++c)
        for (int i = 0; i < a_nodes; ++i)
            if (const int r = a_eq[std::size_t(c) * std::size_t(a_nodes) + std::size_t(i)]; r >= 0) unknown[std::size_t(r)] = int(at(c, i));
    const double mass = o.mass;
    const std::vector<double>* history = mass > 0 ? o.history : nullptr;
    Eigen::VectorXd F;
    std::vector<Eigen::Triplet<double>> trip;
    const int maxIt = std::max(1, o.maxIterations);
    const double tol = o.tolerance > 0 ? o.tolerance : 1e-6;
    a_iterations = 0;
    auto matrix = [&] {
        Eigen::SparseMatrix<double> J(a_neq, a_neq);
        J.setFromTriplets(trip.begin(), trip.end());
        J.makeCompressed();
        return J;
    };
    if (a_linear) {
        // One solve, from F at the values as they are: Newton's first step is
        // the answer. Its matrix kept while the mass, the unknowns and (what
        // they read not varying) its coefficients stay - a fixed step in
        // time's: factored once (the mass to 1e-9, an output time's step
        // rounded).
        const bool kept = a_factored && !a_matrixVaries && a_factoredNeq == a_neq && a_factoredSystem == a_system
                          && std::abs(a_factoredMass - mass) <= 1e-9 * std::max(std::abs(mass), 1e-300);
        if (kept) assemble(mass, history, F, nullptr);
        else {
            assemble(mass, history, F, &trip);
            const Eigen::SparseMatrix<double> J = matrix();
            trip.clear();
            trip.shrink_to_fit();
            if (!factor(J, error)) return false;
            a_factored = true;
            a_factoredMass = mass;
            a_factoredNeq = a_neq;
            a_factoredSystem = a_system;
        }
        const Eigen::VectorXd delta = a_lu->solve(-F);
        if (!delta.allFinite()) {
            *error = tr("%1: its solution is not finite").arg(a_node->name());
            return false;
        }
        for (std::size_t r = 0; r < unknown.size(); ++r) f.values[std::size_t(unknown[r])] += delta(Eigen::Index(r));
        a_iterations = 1;
        f.solved = true;
        project();
        return true;
    }
    for (int it = 0; it < maxIt; ++it) {
        assemble(mass, history, F, &trip);
        if (!factor(matrix(), error)) return false;
        a_factored = false;   // (a nonlinear problem's: the analysis kept, not the matrix)
        const Eigen::VectorXd delta = a_lu->solve(-F);
        if (!delta.allFinite()) {
            *error = tr("%1: Newton's step is not finite").arg(a_node->name());
            return false;
        }
        const double before = F.norm();
        std::vector<double> start(unknown.size());
        double full = 0, was = 0;
        for (std::size_t r = 0; r < unknown.size(); ++r) {
            start[r] = f.values[std::size_t(unknown[r])];
            full = std::max(full, std::abs(delta(Eigen::Index(r))));
            was = std::max(was, std::abs(start[r]));
        }
        double lambda = 1;
        for (int damp = 0; damp < 8; ++damp) {
            for (std::size_t r = 0; r < unknown.size(); ++r) f.values[std::size_t(unknown[r])] = start[r] + lambda * delta(Eigen::Index(r));
            // (The first step whole - a linear problem's answer - and one
            // already within the tolerance.)
            if (damp == 0 && (it == 0 || full <= tol * std::max(was, 1e-30))) break;
            Eigen::VectorXd F2;
            assemble(mass, history, F2, nullptr);
            if (F2.allFinite() && F2.norm() <= before * (1 - 1e-4 * lambda)) break;
            if (damp == 7) break;
            lambda /= 2;
        }
        ++a_iterations;
        double du = 0, size = 0;
        for (std::size_t r = 0; r < unknown.size(); ++r) {
            du = std::max(du, std::abs(lambda * delta(Eigen::Index(r))));
            size = std::max(size, std::abs(f.values[std::size_t(unknown[r])]));
        }
        if (du <= tol * std::max(size, 1e-30) || du == 0) break;
        if (it + 1 == maxIt) {
            *error = tr("%1: Newton's method did not converge in %2 iterations").arg(a_node->name()).arg(maxIt);
            return false;
        }
    }
    f.solved = true;
    project();
    return true;
}

int PdeProblem::solveModes(const SolveOptions& o, int count, double guess, QString* error)
{
    (void)o;
    Field& f = field();
    // The variables only (a rate's rows are left out); homogeneous: the
    // Dirichlet nodes at 0, linearized about the solution as it is (0 when
    // unsolved).
    a_system = a_vars;
    applyDirichlet();
    for (int c = 0; c < a_vars; ++c)
        for (int i = 0; i < a_nodes; ++i)
            if (a_fixed[std::size_t(c) * std::size_t(a_nodes) + std::size_t(i)]) f.values[at(c, i)] = 0;
    Eigen::VectorXd F;
    std::vector<Eigen::Triplet<double>> tj, tm;
    assemble(0, nullptr, F, &tj, &tm);
    const int n = a_neq;
    if (n < 2) {
        *error = tr("%1: too few unknowns for eigenvalues").arg(a_node->name());
        return -1;
    }
    // J x + λ M x = 0: A x = λ M x with A = -J.
    Eigen::SparseMatrix<double> J(n, n), M(n, n);
    J.setFromTriplets(tj.begin(), tj.end());
    M.setFromTriplets(tm.begin(), tm.end());
    const Eigen::SparseMatrix<double> A = -J;
    Eigen::SparseMatrix<double> shifted = A - guess * M;
    shifted.makeCompressed();
    SparseLu<double> lu;
    lu.analyzePattern(shifted);
    lu.factorize(shifted);
    if (lu.info() != Eigen::Success) {
        *error = tr("%1: the shifted matrix is singular - an eigenvalue at %2 itself (search about another)")
                     .arg(a_node->name(), formatNumber(guess));
        return -1;
    }
    const int asked = std::min(std::max(1, count) + 2, n - 1);
    const EigenResult r = largestEigenvalues(
        n, asked,
        [&](const Eigen::VectorXcd& x, Eigen::VectorXcd& y) {
            const Eigen::VectorXd re = M * x.real(), im = M * x.imag();
            const Eigen::VectorXd sr = lu.solve(re), si = lu.solve(im);
            y.resize(n);
            for (int i = 0; i < n; ++i) y(i) = std::complex<double>(sr(i), si(i));
        },
        0, 1e-10, 300, [this] { return cancelled(); });
    if (r.cancelled) {
        *error = tr("cancelled");
        return -1;
    }
    if (!r.error.isEmpty()) a_s.log << tr("Warning: %1: %2").arg(a_node->name(), r.error);
    a_eigen.clear();
    for (std::size_t i = 0; i < r.values.size(); ++i) {
        if (std::abs(r.values[i]) < 1e-300) continue;
        Eigen1 e;
        e.lambda = guess + 1.0 / r.values[i];
        // Its phase: its largest value real.
        Eigen::VectorXcd x = r.vectors[i];
        Eigen::Index big = 0;
        x.cwiseAbs().maxCoeff(&big);
        if (std::abs(x(big)) > 0) x *= std::conj(x(big)) / std::abs(x(big));
        e.x = x.real();
        const double norm = e.x.cwiseAbs().maxCoeff();
        if (norm > 0) e.x /= norm;
        a_eigen.push_back(std::move(e));
    }
    // Those nearest the guess, as many as asked, in order of their real parts.
    std::sort(a_eigen.begin(), a_eigen.end(), [&](const Eigen1& a, const Eigen1& b) { return std::abs(a.lambda - guess) < std::abs(b.lambda - guess); });
    if (int(a_eigen.size()) > count) a_eigen.resize(std::size_t(count));
    std::sort(a_eigen.begin(), a_eigen.end(), [](const Eigen1& a, const Eigen1& b) { return a.lambda.real() < b.lambda.real(); });
    QStringList found;
    for (const Eigen1& e : a_eigen) found << formatNumber(e.lambda.real(), 6);
    a_s.log << tr("%1: eigenvalues %2 (about %3)").arg(a_node->name(), found.join(QLatin1String(", ")), formatNumber(guess));
    f.solved = true;
    return int(a_eigen.size());
}

void PdeProblem::selectMode(int k)
{
    if (k < 0 || k >= int(a_eigen.size())) return;
    Field& f = field();
    a_selected = k;
    std::fill(f.values.begin(), f.values.end(), 0.0);
    a_system = a_vars;
    applyDirichlet();
    for (int c = 0; c < a_vars; ++c)
        for (int i = 0; i < a_nodes; ++i)
            if (const int r = a_eq[std::size_t(c) * std::size_t(a_nodes) + std::size_t(i)]; r >= 0)
                f.values[at(c, i)] = a_eigen[std::size_t(k)].x(r);
    project();
    a_s.setMode(k + 1);
}

void PdeProblem::setGlobals()
{
    const QString p = field().tag + QLatin1Char('.');
    if (a_selected >= 0 && a_selected < int(a_eigen.size())) {
        const std::complex<double> l = a_eigen[std::size_t(a_selected)].lambda;
        a_s.setGlobal(p + QStringLiteral("lambda"), l.real(), Dim(), tr("the eigenvalue: -λ da u + ∇·Γ = F"));
        a_s.setGlobal(p + QStringLiteral("lambda_im"), l.imag(), Dim(), tr("its imaginary part"));
        a_s.setGlobal(p + QStringLiteral("eig"), a_selected + 1, Dim(), tr("the eigenvalue's number, by its real part"));
    }
    a_s.setGlobal(p + QStringLiteral("iterations"), a_iterations, Dim(), tr("Newton's iterations of the last solve (a linear problem's 1)"));
    a_s.setGlobal(p + QStringLiteral("factorizations"), a_factorizations, Dim(),
                  tr("its matrix's factorizations so far in the step: a linear problem's once while its mass stays"));
}

} // namespace

QStringList pdeVariables(const Node& physics)
{
    QStringList out;
    for (const QString& n : physics.text(QStringLiteral("variables")).split(QRegularExpression(QStringLiteral("[,; ]+")), Qt::SkipEmptyParts))
        if (QRegularExpression(QStringLiteral("^[A-Za-z][A-Za-z0-9_]*$")).match(n).hasMatch() && !out.contains(n)) out << n;
    if (out.isEmpty()) out << QStringLiteral("u");
    return out;
}

bool pdeSecondOrder(const Node& physics)
{
    // Any equation with a mass coefficient ea other than 0.
    for (const Node& f : physics.children) {
        if (!f.enabled || (f.type != QLatin1String("pde/coefficient") && f.type != QLatin1String("pde/general"))) continue;
        for (const QString& part : f.text(QStringLiteral("ea")).split(QLatin1Char(';'))) {
            const QString t = part.trimmed();
            bool number = false;
            const double v = t.toDouble(&number);
            if (!t.isEmpty() && !(number && v == 0)) return true;
        }
    }
    return false;
}

std::unique_ptr<Problem> makePdeProblem(Solution& solution, int field)
{
    return std::make_unique<PdeProblem>(solution, field);
}

void pdeLineAt(PointEvaluator& e, int fi, int c, double* u, double* us)
{
    *u = *us = NaN;
    const Field& f = e.solution().fields()[std::size_t(fi)];
    if (!f.fixed || f.fixed->size() < std::size_t(Header) || (*f.fixed)[0] != 1) return;
    const int d = e.domain();
    if (d < 0 || std::size_t(d) >= f.domains.size() || !f.domains[std::size_t(d)]) return;
    const std::vector<double>& fx = *f.fixed;
    const QPointF S(fx[1], fx[2]), E(fx[3], fx[4]);
    const std::size_t n1 = std::size_t(fx[5]), comps = std::size_t(fx[6]);
    if (n1 < 3 || c < 0 || std::size_t(c) >= comps || fx.size() < std::size_t(Header) + n1) return;
    const std::size_t n2 = std::size_t(f.space->size());
    const std::size_t base = comps * n2 + std::size_t(c) * n1;
    if (f.values.size() < base + n1) return;
    const double L = std::hypot(E.x() - S.x(), E.y() - S.y());
    const QPointF pt = e.point() - S;
    const double s = L > 0 ? (pt.x() * (E.x() - S.x()) + pt.y() * (E.y() - S.y())) / L : 0;
    if (s < -1e-9 * L || s > L * (1 + 1e-9)) return;
    const double* z = fx.data() + Header;
    const std::ptrdiff_t at = std::upper_bound(z, z + n1, s) - z;
    std::size_t el = std::size_t(std::max<std::ptrdiff_t>(0, at - 1)) / 2;
    el = std::min(el, (n1 - 1) / 2 - 1);
    const double z0 = z[2 * el], z2 = z[2 * el + 2];
    const double xi = z2 > z0 ? std::clamp((s - z0) / (z2 - z0), 0.0, 1.0) : 0;
    double N[3], dN[3];
    lineShape(xi, N, dN);
    double v = 0, dv = 0;
    for (int k = 0; k < 3; ++k) {
        v += N[k] * f.values[base + 2 * el + std::size_t(k)];
        dv += dN[k] * f.values[base + 2 * el + std::size_t(k)];
    }
    *u = v + e.shiftOf(fi, c, 0);
    *us = (z2 > z0 ? dv / (z2 - z0) : 0) + e.shiftOf(fi, c, 1);
}

} // namespace qucs_s::fem::detail
