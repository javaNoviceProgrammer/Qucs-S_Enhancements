/*
 * fem_elastic.cpp - solid mechanics: linear elasticity in plane stress,
 *                   plane strain or about an axis, its displacement (u, v)
 *                   on Lagrange elements; loads on boundaries and in
 *                   domains, a thermal strain α (T - Tref), constraints
 *                   fixed, prescribed or rolling
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_problem.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace qucs_s::fem::detail {

namespace {

/// A boundary's own constraint: the last feature's.
struct Constraint {
    enum Kind { None, Fixed, Displacement, Roller };
    Kind kind = None;
    bool x = false, y = false;   // which components a displacement sets
    Expression u0, v0;
};

/// A load on a boundary: a force per area, or a pressure.
struct Load {
    bool pressure = false;
    Expression fx, fy, p;
};

class ElasticProblem : public Problem
{
public:
    ElasticProblem(Solution& solution, int field) : Problem(solution, field) {}

    bool setup(QString* error) override;
    bool solve(const SolveOptions& options, QString* error) override;
    void setGlobals() override;

private:
    void elementMatrices();
    bool buildEquations(QString* error);
    void constrainedValues();
    int n() const { return field().space->size(); }
    const Field& field() const { return a_s.fields()[std::size_t(a_fi)]; }
    using Problem::field;

    bool a_planeStress = false;
    std::vector<Constraint> a_constraints;      // by boundary
    std::vector<std::vector<Load>> a_loads;     // by boundary
    std::vector<std::vector<std::pair<Expression, Expression>>> a_body;   // by domain
    std::vector<Expression> a_thermalT, a_thermalRef;   // by domain (the expressions dT is made of)
    double a_emax = 0;

    std::vector<int> a_eq;                      // by component * n + dof: its equation, -1 none, -3 fixed
    std::vector<int> a_constraintOf;            // by component * n + dof: the boundary that fixes it (-2: the axis)
    std::vector<double> a_fixed;                // their values
    int a_neq = 0;
    std::vector<double> a_K, a_F;               // by element: (2 per)² and 2 per
    struct Penalty {
        int edge;
        double K[36];                           // (2 * 3)²: the edge's dofs, x then y of each
    };
    std::vector<Penalty> a_penalties;           // an oblique roller's edges
    struct EdgeLoad {
        int edge;
        double F[6];
    };
    std::vector<EdgeLoad> a_edgeLoads;
    LinearSystem a_system;
    bool a_patternReady = false;
    bool a_varies = true;
    double a_rx = 0, a_ry = 0, a_energy = 0;
};

bool ElasticProblem::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field& f = field();
    const Model& model = a_s.model();
    const Topology& topo = a_s.topology();
    a_planeStress = !a_s.axisymmetric() && a_node->text(QStringLiteral("model")) == QLatin1String("planestress");
    QString why;
    const QVector<int> domains = resolveSelection(topo, model, Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(topo.domains.size(), 0);
    for (int d : domains) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    const std::vector<const Node*> material = materials();
    Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const std::size_t nd = topo.domains.size();
    coef.c.assign(nd, Expression());
    coef.sources.assign(nd, {});
    coef.sourceScale.assign(nd, 1.0);
    coef.capacity.assign(nd, Expression());
    for (const char* key : {"E", "nu", "alpha", "dT"}) coef.named[QString::fromLatin1(key)].assign(nd, Expression());
    a_body.assign(nd, {});
    a_constraints.assign(topo.boundaries.size(), Constraint());
    a_loads.assign(topo.boundaries.size(), {});
    std::vector<const Node*> elasticOf(nd, nullptr);
    for (const Node& feature : a_node->children) {
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k) continue;
        const QVector<int> picked = resolveSelection(topo, model, k->selection, feature.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(feature.name(), why));
        const QString t = feature.type;
        if (t == QLatin1String("solid/elastic")) {
            for (int d : picked) elasticOf[std::size_t(d)] = &feature;
        } else if (t == QLatin1String("solid/bodyload")) {
            const Expression fx = compile(feature.text(QStringLiteral("fx")), feature, QStringLiteral("fx"));
            const Expression fy = compile(feature.text(QStringLiteral("fy")), feature, QStringLiteral("fy"));
            if (fx.isValid() && fy.isValid())
                for (int d : picked) a_body[std::size_t(d)].emplace_back(fx, fy);
        } else if (t == QLatin1String("solid/fixed") || t == QLatin1String("solid/displacement") || t == QLatin1String("solid/roller")
                   || t == QLatin1String("solid/free")) {
            Constraint c;
            if (t == QLatin1String("solid/fixed")) {
                c.kind = Constraint::Fixed;
                c.x = c.y = true;
            } else if (t == QLatin1String("solid/roller")) {
                c.kind = Constraint::Roller;
            } else if (t == QLatin1String("solid/displacement")) {
                c.kind = Constraint::Displacement;
                c.x = feature.flag(QStringLiteral("x"));
                c.y = feature.flag(QStringLiteral("y"));
                if (c.x) c.u0 = compile(feature.text(QStringLiteral("u0")), feature, QStringLiteral("u0"), false);
                if (c.y) c.v0 = compile(feature.text(QStringLiteral("v0")), feature, QStringLiteral("v0"), false);
                if (!c.x && !c.y) continue;
            }
            for (int b : picked) a_constraints[std::size_t(b)] = c;
        } else if (t == QLatin1String("solid/load")) {
            Load l;
            l.pressure = feature.text(QStringLiteral("kind")) == QLatin1String("pressure");
            if (l.pressure) l.p = compile(feature.text(QStringLiteral("p")), feature, QStringLiteral("p"));
            else {
                l.fx = compile(feature.text(QStringLiteral("Fx")), feature, QStringLiteral("Fx"));
                l.fy = compile(feature.text(QStringLiteral("Fy")), feature, QStringLiteral("Fy"));
            }
            if ((l.pressure && l.p.isValid()) || (!l.pressure && l.fx.isValid() && l.fy.isValid()))
                for (int b : picked) a_loads[std::size_t(b)].push_back(l);
        }
    }
    a_emax = 0;
    a_varies = false;
    for (int d = 0; d < int(nd); ++d) {
        if (!f.domains[std::size_t(d)]) continue;
        const Node* feat = elasticOf[std::size_t(d)];
        const QString object = topo.objectNames.value(topo.domains[std::size_t(d)].owner);
        if (!feat) {
            problem(tr("domain %1 (%2) has no Linear Elastic Material").arg(d + 1).arg(object));
            continue;
        }
        const bool user = feat->text(QStringLiteral("source")) == QLatin1String("user");
        const Node* m = material[std::size_t(d)];
        auto property = [&](const QString& key, const QString& name, Expression* out) {
            QString text;
            const Node* where = feat;
            if (user) text = feat->text(key);
            else if (!m) {
                problem(tr("domain %1 (%2) has no material: its %3 is needed").arg(d + 1).arg(object, name));
                return;
            } else {
                text = materialProperty(*m, key);
                where = m;
            }
            if (text.trimmed().isEmpty()) {
                problem(user ? tr("domain %1 (%2): its %3 is not given").arg(d + 1).arg(object, name)
                             : tr("domain %1 (%2): its material, %3, has no %4").arg(d + 1).arg(object, m->name(), name));
                return;
            }
            *out = compile(text, *where, key);
        };
        Expression E, nu;
        property(QStringLiteral("E"), tr("Young's modulus"), &E);
        property(QStringLiteral("nu"), tr("Poisson's ratio"), &nu);
        if (E.isValid() && E.isConstant() && !(E.constant() > 0)) problem(tr("domain %1: its Young's modulus must be more than 0").arg(d + 1));
        if (nu.isValid() && nu.isConstant() && !(nu.constant() > -1 && nu.constant() < 0.5))
            problem(tr("domain %1: its Poisson's ratio must be between -1 and 0.5").arg(d + 1));
        if (E.isValid() && E.isConstant()) a_emax = std::max(a_emax, E.constant());
        coef.named[QStringLiteral("E")][std::size_t(d)] = E;
        coef.named[QStringLiteral("nu")][std::size_t(d)] = nu;
        if (feat->flag(QStringLiteral("thermal"))) {
            Expression alpha;
            property(QStringLiteral("alpha"), tr("coefficient of thermal expansion"), &alpha);
            const QString T = feat->text(QStringLiteral("T")), ref = feat->text(QStringLiteral("Tref"));
            const Expression te = compile(T, *feat, QStringLiteral("T"));
            const Expression re = compile(ref, *feat, QStringLiteral("Tref"), false);
            if (te.isValid() && re.isValid()) {
                coef.named[QStringLiteral("alpha")][std::size_t(d)] = alpha;
                coef.named[QStringLiteral("dT")][std::size_t(d)] =
                    Expression::compile(QStringLiteral("(") + T + QStringLiteral(")-(") + ref + QLatin1Char(')'), a_s.pointScope());
            }
        } else {
            coef.named[QStringLiteral("alpha")][std::size_t(d)] = Expression::compile(QStringLiteral("0"), a_s.pointScope());
            coef.named[QStringLiteral("dT")][std::size_t(d)] = Expression::compile(QStringLiteral("0[K]"), a_s.pointScope());
        }
        for (const char* key : {"E", "nu", "alpha", "dT"}) {
            const Expression& e = coef.named[QString::fromLatin1(key)][std::size_t(d)];
            a_varies = a_varies || (e.isValid() && a_s.varies(e));
        }
        for (const auto& [fx, fy] : a_body[std::size_t(d)]) a_varies = a_varies || a_s.varies(fx) || a_s.varies(fy);
    }
    if (a_emax == 0) a_emax = 1e11;
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    a_patternReady = false;
    return true;
}

bool ElasticProblem::buildEquations(QString* error)
{
    Field& f = field();
    const Space& sp = *f.space;
    const Mesh& mesh = a_s.mesh();
    const Topology& topo = a_s.topology();
    const int nn = n();
    const int per = sp.perElement();
    a_eq.assign(std::size_t(2 * nn), -1);
    a_constraintOf.assign(std::size_t(2 * nn), -1);
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = mesh.domain[std::size_t(t)];
        if (dom < 0 || !f.domains[std::size_t(dom)]) continue;
        for (int i = 0; i < per; ++i)
            for (int c = 0; c < 2; ++c) a_eq[std::size_t(c * nn + sp.dofs(t)[i])] = -2;
    }
    a_penalties.clear();
    int constrained = 0;
    for (int e = 0; e < int(mesh.edges.size()); ++e) {
        const Mesh::Edge& me = mesh.edges[std::size_t(e)];
        const Constraint& c = a_constraints[std::size_t(me.boundary)];
        if (c.kind == Constraint::None) continue;
        bool fx = c.x, fy = c.y;
        if (c.kind == Constraint::Roller) {
            // Its normal along an axis: that component fixed; else held by a
            // stiff spring along its normal.
            const Topology::Boundary& b = topo.boundaries[std::size_t(me.boundary)];
            const Curve* curve = b.curve >= 0 ? &topo.curves[std::size_t(b.curve)] : nullptr;
            QPointF dir = mesh.nodes[std::size_t(me.b)] - mesh.nodes[std::size_t(me.a)];
            const double len = std::hypot(dir.x(), dir.y());
            if (len > 0) dir /= len;
            const bool straight = curve && curve->kind == Curve::Line;
            if (straight && std::abs(dir.y()) < 1e-9) fy = true;
            else if (straight && std::abs(dir.x()) < 1e-9) fx = true;
            else {
                // n nᵀ ∫ N_i N_j, scaled to be far stiffer than the solid.
                Penalty p{e, {}};
                const QPointF normal(dir.y(), -dir.x());
                const int ne = sp.order() == 1 ? 2 : 3;
                const double kappa = 1e5 * a_emax / (len * mesh.unitScale);
                for (const auto& q : lineRule(sp.order() + 2)) {
                    double jac = 0;
                    const QPointF x = sp.edgePoint(e, q[0], &jac);
                    const double s = q[0];
                    double N[3];
                    if (ne == 2) {
                        N[0] = 1 - s;
                        N[1] = s;
                    } else {
                        N[0] = (1 - s) * (1 - 2 * s);
                        N[1] = s * (2 * s - 1);
                        N[2] = 4 * s * (1 - s);
                    }
                    const double w = q[1] * jac * a_s.weight(x) * kappa;
                    const double nn2[2][2] = {{normal.x() * normal.x(), normal.x() * normal.y()}, {normal.x() * normal.y(), normal.y() * normal.y()}};
                    for (int i = 0; i < ne; ++i)
                        for (int j = 0; j < ne; ++j)
                            for (int a = 0; a < 2; ++a)
                                for (int bb = 0; bb < 2; ++bb) p.K[(2 * i + a) * 6 + 2 * j + bb] += w * N[i] * N[j] * nn2[a][bb];
                }
                a_penalties.push_back(p);
                ++constrained;
                continue;
            }
        }
        for (int dof : sp.edgeDofs(e)) {
            if (dof < 0) continue;
            for (int comp = 0; comp < 2; ++comp) {
                if (!(comp == 0 ? fx : fy)) continue;
                const std::size_t k = std::size_t(comp * nn + dof);
                if (a_eq[k] == -1) continue;
                a_eq[k] = -3;
                a_constraintOf[k] = me.boundary;
                ++constrained;
            }
        }
    }
    // About the axis: no radial displacement on it.
    if (a_s.axisymmetric()) {
        const QRectF b = mesh.bounds();
        const double tol = 1e-9 * std::max(b.width(), b.height()) * mesh.unitScale;
        for (int dof = 0; dof < nn; ++dof)
            if (a_eq[std::size_t(dof)] != -1 && std::abs(sp.point(dof).x()) <= tol) {
                a_eq[std::size_t(dof)] = -3;
                a_constraintOf[std::size_t(dof)] = -2;
            }
    }
    if (constrained == 0) {
        *error = tr("%1: nothing holds the solid (no Fixed Constraint, Prescribed Displacement or Roller): it may move as a whole")
                     .arg(a_node->name());
        return false;
    }
    a_neq = 0;
    for (std::size_t k = 0; k < a_eq.size(); ++k)
        if (a_eq[k] == -2) a_eq[k] = a_neq++;
    if (a_neq == 0) {
        *error = tr("%1: nothing to solve - every displacement is set").arg(a_node->name());
        return false;
    }
    a_system.setPattern(a_neq, [&](const std::function<void(const int*, int)>& element) {
        int local[12];
        for (int t = 0; t < sp.elements(); ++t) {
            const int dom = mesh.domain[std::size_t(t)];
            if (dom < 0 || !f.domains[std::size_t(dom)]) continue;
            for (int i = 0; i < per; ++i)
                for (int c = 0; c < 2; ++c) local[2 * i + c] = a_eq[std::size_t(c * nn + sp.dofs(t)[i])];
            element(local, 2 * per);
        }
        for (const Penalty& p : a_penalties) {
            const std::array<int, 3> d = sp.edgeDofs(p.edge);
            int count = 0;
            for (int i = 0; i < 3; ++i) {
                if (d[std::size_t(i)] < 0) continue;
                for (int c = 0; c < 2; ++c) local[count++] = a_eq[std::size_t(c * nn + d[std::size_t(i)])];
            }
            element(local, count);
        }
    });
    a_patternReady = true;
    return true;
}

void ElasticProblem::constrainedValues()
{
    const Space& sp = *field().space;
    const int nn = n();
    a_fixed.assign(a_eq.size(), 0);
    std::vector<double> values(std::size_t(a_s.pointScope().slotCount()), NaN);
    const auto& defs = a_s.definitions();
    for (std::size_t k = 0; k < a_eq.size(); ++k) {
        if (a_eq[k] != -3 || a_constraintOf[k] < 0) continue;
        const Constraint& c = a_constraints[std::size_t(a_constraintOf[k])];
        if (c.kind != Constraint::Displacement) continue;
        const int comp = int(k) / nn, dof = int(k) % nn;
        const Expression& e = comp == 0 ? c.u0 : c.v0;
        if (!e.isValid()) continue;
        if (e.isConstant()) {
            a_fixed[k] = e.constant();
            continue;
        }
        const QPointF p = sp.point(dof);
        for (int s = 0; s < int(defs.size()); ++s) {
            const auto kd = defs[std::size_t(s)].kind;
            values[std::size_t(s)] = kd == Solution::VariableDef::X ? p.x() : kd == Solution::VariableDef::Y ? p.y()
                                     : kd == Solution::VariableDef::Time                                  ? a_s.time()
                                                                                                           : NaN;
        }
        a_fixed[k] = e.eval(values.data());
    }
}

void ElasticProblem::elementMatrices()
{
    Field& fld = field();
    const Space& sp = *fld.space;
    const Mesh& mesh = a_s.mesh();
    const Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const std::vector<Expression>& Es = coef.named.at(QStringLiteral("E"));
    const std::vector<Expression>& nus = coef.named.at(QStringLiteral("nu"));
    const std::vector<Expression>& alphas = coef.named.at(QStringLiteral("alpha"));
    const std::vector<Expression>& dTs = coef.named.at(QStringLiteral("dT"));
    const int per = sp.perElement();
    const int m = 2 * per;
    const int nt = sp.elements();
    const bool axi = a_s.axisymmetric();
    const int rows = axi ? 4 : 3;
    a_K.assign(std::size_t(nt) * std::size_t(m * m), 0);
    a_F.assign(std::size_t(nt) * std::size_t(m), 0);
    parallelFor(nt, [&](int begin, int end) {
        PointEvaluator ev(a_s);
        for (int t = begin; t < end; ++t) {
            const int dom = mesh.domain[std::size_t(t)];
            if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
            const std::size_t d = std::size_t(dom);
            const TriangleRule& rule = triangleRule(sp.curved(t) || axi ? 5 : sp.order() == 1 ? 2 : 4);
            double* K = &a_K[std::size_t(t) * std::size_t(m * m)];
            double* F = &a_F[std::size_t(t) * std::size_t(m)];
            Shape s;
            double gx[6], gy[6];
            for (const auto& q : rule.points) {
                shapeAt(sp.order(), q[0], q[1], s);
                const Space::Map map = sp.map(t, q[0], q[1]);
                const double r = map.x.x();
                const double w = q[2] * std::abs(map.det) * a_s.weight(map.x);
                ev.moveTo(t, q[0], q[1]);
                const double E = ev.eval(Es[d]), nu = ev.eval(nus[d]);
                const double alpha = alphas[d].isValid() ? ev.eval(alphas[d]) : 0;
                const double dT = dTs[d].isValid() ? ev.eval(dTs[d]) : 0;
                const double th = (std::isfinite(alpha) ? alpha : 0) * (std::isfinite(dT) ? dT : 0);
                double fx = 0, fy = 0;
                for (const auto& [ex, ey] : a_body[d]) {
                    fx += ev.eval(ex);
                    fy += ev.eval(ey);
                }
                // D, and the free thermal strain ε0 it acts on.
                double D[4][4] = {};
                double e0[4] = {};
                const double mu = E / (2 * (1 + nu));
                if (a_planeStress) {
                    const double f = E / (1 - nu * nu);
                    D[0][0] = D[1][1] = f;
                    D[0][1] = D[1][0] = f * nu;
                    D[2][2] = mu;
                    e0[0] = e0[1] = th;
                } else {
                    const double lambda = E * nu / ((1 + nu) * (1 - 2 * nu));
                    for (int i = 0; i < (axi ? 3 : 2); ++i)
                        for (int j = 0; j < (axi ? 3 : 2); ++j) D[i][j] = lambda + (i == j ? 2 * mu : 0);
                    D[rows - 1][rows - 1] = mu;
                    if (axi) e0[0] = e0[1] = e0[2] = th;
                    else e0[0] = e0[1] = (1 + nu) * th;
                }
                double sigma0[4] = {};   // D ε0
                for (int i = 0; i < rows; ++i)
                    for (int j = 0; j < rows; ++j) sigma0[i] += D[i][j] * e0[j];
                // B: a column for each (node, component).
                double B[4][12] = {};
                for (int i = 0; i < per; ++i) {
                    Space::gradient(map, s.dxi[i], s.deta[i], gx[i], gy[i]);
                    B[0][2 * i] = gx[i];
                    B[1][2 * i + 1] = gy[i];
                    if (axi) {
                        B[2][2 * i] = r > 0 ? s.n[i] / r : 0;
                        B[3][2 * i] = gy[i];
                        B[3][2 * i + 1] = gx[i];
                    } else {
                        B[2][2 * i] = gy[i];
                        B[2][2 * i + 1] = gx[i];
                    }
                }
                double DB[4][12];
                for (int i = 0; i < rows; ++i)
                    for (int c = 0; c < m; ++c) {
                        double v = 0;
                        for (int k = 0; k < rows; ++k) v += D[i][k] * B[k][c];
                        DB[i][c] = v;
                    }
                for (int a = 0; a < m; ++a) {
                    for (int b = a; b < m; ++b) {
                        double v = 0;
                        for (int k = 0; k < rows; ++k) v += B[k][a] * DB[k][b];
                        K[a * m + b] += w * v;
                        if (b != a) K[b * m + a] += w * v;
                    }
                    double ft = 0;
                    for (int k = 0; k < rows; ++k) ft += B[k][a] * sigma0[k];
                    F[a] += w * ft;
                }
                for (int i = 0; i < per; ++i) {
                    F[2 * i] += w * fx * s.n[i];
                    F[2 * i + 1] += w * fy * s.n[i];
                }
            }
        }
    });
    // Loads on the boundaries.
    a_edgeLoads.clear();
    PointEvaluator ev(a_s);
    for (int e = 0; e < int(mesh.edges.size()); ++e) {
        const Mesh::Edge& me = mesh.edges[std::size_t(e)];
        const std::vector<Load>& loads = a_loads[std::size_t(me.boundary)];
        if (loads.empty()) continue;
        int tri = -1;
        bool left = true;
        for (int side : {me.left, me.right})
            if (side >= 0 && fld.domains[std::size_t(mesh.domain[std::size_t(side)])]) {
                tri = side;
                left = side == me.left;
                break;
            }
        if (tri < 0) continue;
        const std::array<int, 3>& tv = mesh.triangles[std::size_t(tri)];
        const int k = Space::localEdge(tv, me.a, me.b);
        const bool same = tv[std::size_t(k)] == me.a;
        EdgeLoad el{e, {}};
        const int ne = sp.order() == 1 ? 2 : 3;
        for (const auto& q : lineRule(sp.order() + 2)) {
            double jac = 0;
            const QPointF x = sp.edgePoint(e, q[0], &jac);
            // The outward normal: the solid on the edge's left, it points right.
            const double ds = 1e-6;
            const QPointF x2 = sp.edgePoint(e, std::min(1.0, q[0] + ds));
            const QPointF x1 = sp.edgePoint(e, std::max(0.0, q[0] - ds));
            QPointF tangent = x2 - x1;
            const double len = std::hypot(tangent.x(), tangent.y());
            if (len > 0) tangent /= len;
            const QPointF normal = left ? QPointF(tangent.y(), -tangent.x()) : QPointF(-tangent.y(), tangent.x());
            const double s = same ? q[0] : 1 - q[0];
            const double xi = k == 0 ? s : k == 1 ? 1 - s : 0;
            const double eta = k == 0 ? 0 : k == 1 ? s : 1 - s;
            ev.moveTo(tri, xi, eta);
            double tx = 0, ty = 0;
            for (const Load& l : loads) {
                if (l.pressure) {
                    const double p = ev.eval(l.p);
                    tx -= p * normal.x();
                    ty -= p * normal.y();
                } else {
                    tx += ev.eval(l.fx);
                    ty += ev.eval(l.fy);
                }
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
            for (int i = 0; i < ne; ++i) {
                el.F[2 * i] += w * tx * N[i];
                el.F[2 * i + 1] += w * ty * N[i];
            }
        }
        a_edgeLoads.push_back(el);
    }
}

bool ElasticProblem::solve(const SolveOptions& o, QString* error)
{
    Field& fld = field();
    const Space& sp = *fld.space;
    const int nn = n();
    const int per = sp.perElement();
    const int m = 2 * per;
    if (!a_patternReady && !buildEquations(error)) return false;
    elementMatrices();
    constrainedValues();
    a_system.clear();
    const Mesh& mesh = a_s.mesh();
    auto eqOf = [&](int dof, int c) { return a_eq[std::size_t(c * nn + dof)]; };
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = mesh.domain[std::size_t(t)];
        if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
        const double* K = &a_K[std::size_t(t) * std::size_t(m * m)];
        const int* dofs = sp.dofs(t);
        for (int a = 0; a < m; ++a) {
            const int qa = eqOf(dofs[a / 2], a % 2);
            if (qa < 0) continue;
            for (int b = 0; b < m; ++b) {
                const int qb = eqOf(dofs[b / 2], b % 2);
                if (qb >= 0) a_system.add(qa, qb, K[a * m + b]);
            }
        }
    }
    auto penaltyDofs = [&](const Penalty& p, int* local) {
        const std::array<int, 3> d = sp.edgeDofs(p.edge);
        for (int i = 0; i < 3; ++i)
            for (int c = 0; c < 2; ++c) local[2 * i + c] = d[std::size_t(i)] < 0 ? -1 : c * nn + d[std::size_t(i)];
    };
    for (const Penalty& p : a_penalties) {
        int local[6];
        penaltyDofs(p, local);
        for (int a = 0; a < 6; ++a) {
            if (local[a] < 0 || a_eq[std::size_t(local[a])] < 0) continue;
            for (int b = 0; b < 6; ++b)
                if (local[b] >= 0 && a_eq[std::size_t(local[b])] >= 0) a_system.add(a_eq[std::size_t(local[a])], a_eq[std::size_t(local[b])], p.K[a * 6 + b]);
        }
    }
    if (!a_system.factorize(true, o.solver, o.tolerance, a_node->name(), &a_s.log, error)) {
        if (error->contains(QLatin1String("singular")))
            *error = tr("%1: the solid is not held enough - it can move or turn as a whole (fix more of it, or a Roller more)")
                         .arg(a_node->name());
        return false;
    }
    // u: the constrained values, the rest from the residual F - K u.
    std::vector<double> u(std::size_t(2 * nn), 0);
    for (std::size_t k = 0; k < u.size(); ++k)
        if (a_eq[k] == -3) u[k] = a_fixed[k];
    Eigen::VectorXd r = Eigen::VectorXd::Zero(a_neq);
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = mesh.domain[std::size_t(t)];
        if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
        const double* K = &a_K[std::size_t(t) * std::size_t(m * m)];
        const double* F = &a_F[std::size_t(t) * std::size_t(m)];
        const int* dofs = sp.dofs(t);
        for (int a = 0; a < m; ++a) {
            const int qa = eqOf(dofs[a / 2], a % 2);
            if (qa < 0) continue;
            double v = F[a];
            for (int b = 0; b < m; ++b) v -= K[a * m + b] * u[std::size_t((b % 2) * nn + dofs[b / 2])];
            r[qa] += v;
        }
    }
    for (const EdgeLoad& el : a_edgeLoads) {
        const std::array<int, 3> d = sp.edgeDofs(el.edge);
        for (int i = 0; i < 3; ++i) {
            if (d[std::size_t(i)] < 0) continue;
            for (int c = 0; c < 2; ++c) {
                const int q = eqOf(d[std::size_t(i)], c);
                if (q >= 0) r[q] += el.F[2 * i + c];
            }
        }
    }
    Eigen::VectorXd delta;
    if (!a_system.solve(r, delta, a_node->name(), error)) return false;
    for (std::size_t k = 0; k < u.size(); ++k)
        if (a_eq[k] >= 0) u[k] += delta[a_eq[k]];
    fld.values = u;
    fld.solved = true;
    // The reactions: K u - F where it is held (springs too), summed.
    a_rx = a_ry = 0;
    std::vector<double> reaction(u.size(), 0);
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = mesh.domain[std::size_t(t)];
        if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
        const double* K = &a_K[std::size_t(t) * std::size_t(m * m)];
        const double* F = &a_F[std::size_t(t) * std::size_t(m)];
        const int* dofs = sp.dofs(t);
        for (int a = 0; a < m; ++a) {
            const std::size_t ka = std::size_t((a % 2) * nn + dofs[a / 2]);
            if (a_eq[ka] != -3) continue;
            double v = -F[a];
            for (int b = 0; b < m; ++b) v += K[a * m + b] * u[std::size_t((b % 2) * nn + dofs[b / 2])];
            reaction[ka] += v;
        }
    }
    for (const EdgeLoad& el : a_edgeLoads) {
        const std::array<int, 3> d = sp.edgeDofs(el.edge);
        for (int i = 0; i < 3; ++i)
            for (int c = 0; c < 2; ++c)
                if (d[std::size_t(i)] >= 0 && a_eq[std::size_t(c * nn + d[std::size_t(i)])] == -3)
                    reaction[std::size_t(c * nn + d[std::size_t(i)])] -= el.F[2 * i + c];
    }
    for (std::size_t k = 0; k < u.size(); ++k) (int(k) < nn ? a_rx : a_ry) += reaction[k];
    for (const Penalty& p : a_penalties) {
        int local[6];
        penaltyDofs(p, local);
        for (int a = 0; a < 6; ++a) {
            if (local[a] < 0) continue;
            double v = 0;
            for (int b = 0; b < 6; ++b)
                if (local[b] >= 0) v += p.K[a * 6 + b] * u[std::size_t(local[b])];
            (a % 2 == 0 ? a_rx : a_ry) += v;
        }
    }
    // The strain energy: ∫ Ws.
    a_energy = 0;
    const Expression ws = Expression::compile(fld.tag + QStringLiteral(".Ws"), a_s.pointScope());
    if (ws.isValid()) {
        PointEvaluator ev(a_s);
        for (int t = 0; t < sp.elements(); ++t) {
            const int dom = mesh.domain[std::size_t(t)];
            if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
            for (const auto& q : triangleRule(sp.order() == 1 ? 2 : 4).points) {
                const Space::Map map = sp.map(t, q[0], q[1]);
                ev.moveTo(t, q[0], q[1]);
                const double v = ev.eval(ws);
                if (std::isfinite(v)) a_energy += q[2] * std::abs(map.det) * a_s.weight(map.x) * v;
            }
        }
    }
    Q_UNUSED(per);
    return true;
}

void ElasticProblem::setGlobals()
{
    const Field& f = field();
    const QString p = f.tag + QLatin1Char('.');
    const int nn = n();
    double dmax = 0;
    for (int dof = 0; dof < nn; ++dof) {
        const double d = std::hypot(f.values[std::size_t(dof)], f.values[std::size_t(nn + dof)]);
        if (std::isfinite(d)) dmax = std::max(dmax, d);
    }
    const bool axi = a_s.axisymmetric();
    a_s.setGlobal(p + QStringLiteral("dmax"), dmax, Dim::length(), tr("largest displacement"));
    a_s.setGlobal(p + QStringLiteral("W"), a_energy, Dim::of(2, 1, -2), tr("strain energy"));
    if (!axi) a_s.setGlobal(p + QStringLiteral("Rx"), a_rx, Dim::of(1, 1, -2), tr("reaction force where it is held, x component (over the thickness)"));
    a_s.setGlobal(p + (axi ? QStringLiteral("Rz") : QStringLiteral("Ry")), a_ry, Dim::of(1, 1, -2),
                  axi ? tr("reaction force where it is held, along the axis (all round it)")
                      : tr("reaction force where it is held, y component (over the thickness)"));
}

} // namespace

std::unique_ptr<Problem> makeElasticProblem(Solution& solution, int field)
{
    return std::make_unique<ElasticProblem>(solution, field);
}

} // namespace qucs_s::fem::detail
