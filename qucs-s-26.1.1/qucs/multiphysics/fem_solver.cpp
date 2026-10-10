/*
 * fem_solver.cpp - the multiphysics solver: assembly, solution, sweeps
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_solver.h"

#include "fem_materials.h"

#include <Eigen/Dense>
#include <Eigen/IterativeLinearSolvers>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseLU>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <thread>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Solver", text);
}

constexpr double Eps0 = 8.8541878188e-12;
const double NaN = std::numeric_limits<double>::quiet_NaN();

/// Runs \a work(begin, end) on parts of [0, n) in threads.
void parallelFor(int n, const std::function<void(int, int)>& work)
{
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    if (n < 2000 || threads == 1) {
        work(0, n);
        return;
    }
    std::vector<std::thread> pool;
    const int chunk = (n + threads - 1) / threads;
    for (int k = 0; k < threads; ++k) {
        const int b = k * chunk, e = std::min(n, b + chunk);
        if (b >= e) break;
        pool.emplace_back(work, b, e);
    }
    for (std::thread& t : pool) t.join();
}

} // namespace

// ------------------------------------------------------------------ Solution

Solution::Solution(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Mesh> mesh)
    : a_model(model), a_params(std::move(parameters)), a_mesh(std::move(mesh))
{
    QString why;
    const std::optional<double> d = evaluateConstant(a_model.component().text(QStringLiteral("thickness")), a_params->scope,
                                                     Dim::length(), &why);
    a_thickness = d && *d > 0 ? *d : 1.0;
    std::map<int, std::shared_ptr<const Space>> spaces;
    for (const Node* p : a_model.physics()) {
        if (!p->enabled) continue;
        Field f;
        f.tag = p->tag;
        f.type = p->type;
        const bool heat = p->type == QLatin1String("heat");
        f.variable = heat ? QStringLiteral("T") : QStringLiteral("V");
        f.dim = heat ? Dim::temperature() : Dim::voltage();
        f.initial = heat ? 293.15 : 0.0;
        const int order = p->text(QStringLiteral("order")) == QLatin1String("1") ? 1 : 2;
        auto it = spaces.find(order);
        if (it == spaces.end()) it = spaces.emplace(order, std::make_shared<Space>(a_mesh, order)).first;
        f.space = it->second;
        f.values.assign(std::size_t(f.space->size()), f.initial);
        f.domains.assign(a_mesh->topology->domains.size(), 0);
        a_fields.push_back(std::move(f));
    }
    a_coefficients.resize(a_fields.size());
    defineVariables();
}

int Solution::fieldIndex(const QString& tag) const
{
    for (int i = 0; i < int(a_fields.size()); ++i)
        if (a_fields[std::size_t(i)].tag == tag) return i;
    return -1;
}

int Solution::define(const QString& name, const Dim& dim, const QString& description, VariableDef::Kind kind, int field,
                     std::function<double(PointEvaluator&)> derived)
{
    if (a_pointScope->variable(name) >= 0) return a_pointScope->variable(name);
    const int slot = a_pointScope->addVariable(name, dim);
    if (int(a_defs.size()) <= slot) a_defs.resize(std::size_t(slot) + 1);
    VariableDef& d = a_defs[std::size_t(slot)];
    d.info = {name, dim, description};
    d.kind = kind;
    d.field = field;
    d.derived = std::move(derived);
    return slot;
}

namespace {

/// A physics as its variables need it.
struct FieldInfo {
    QString tag, type, variable;
    Dim dim;
};

using DefineFunction = std::function<int(const QString&, const Dim&, const QString&, Solution::VariableDef::Kind, int,
                                         std::function<double(PointEvaluator&)>)>;

/// The variables of a solution of \a fields, each to \a defineVariable:
/// x, y, the fields and what follows from them.
void declareVariables(const std::vector<FieldInfo>& fields, const DefineFunction& defineVariable)
{
    using K = Solution::VariableDef::Kind;
    auto define = [&](const QString& name, const Dim& dim, const QString& description, K kind, int field = -1,
                      std::function<double(PointEvaluator&)> derived = {}) {
        return defineVariable(name, dim, description, kind, field, std::move(derived));
    };
    define(QStringLiteral("x"), Dim::length(), tr("x coordinate"), K::X);
    define(QStringLiteral("y"), Dim::length(), tr("y coordinate"), K::Y);
    define(QStringLiteral("dom"), Dim(), tr("the domain's number"), K::DomainNumber);
    define(QStringLiteral("h"), Dim::length(), tr("the mesh element's size"), K::Size);
    const Dim efield = Dim::of(1, 1, -3, -1), dfield = Dim::of(-2, 0, 1, 1), jfield = Dim::of(-2, 0, 0, 1);
    const Dim power = Dim::of(-1, 1, -3), gradT = Dim::of(-1, 0, 0, 0, 1), heatFlux = Dim::of(0, 1, -3);
    for (int f = 0; f < int(fields.size()); ++f) {
        const FieldInfo& field = fields[std::size_t(f)];
        const QString p = field.tag + QLatin1Char('.');
        define(p + field.variable, field.dim, tr("the dependent variable"), K::Value, f, {});
        // Its plain name too (V, T), when no physics before has it.
        define(field.variable, field.dim, tr("the dependent variable of %1").arg(field.tag), K::Value, f, {});
        if (field.type == QLatin1String("electrostatics") || field.type == QLatin1String("currents")) {
            const int ex = define(p + QStringLiteral("Ex"), efield, tr("electric field, x component"), K::GradX, f);
            const int ey = define(p + QStringLiteral("Ey"), efield, tr("electric field, y component"), K::GradY, f);
            const int ne = define(p + QStringLiteral("normE"), efield, tr("electric field, norm"), K::GradNorm, f);
            if (field.type == QLatin1String("electrostatics")) {
                const int er = define(p + QStringLiteral("epsr"), Dim(), tr("relative permittivity"), K::Coefficient, f);
                const int dx = define(p + QStringLiteral("Dx"), dfield, tr("electric displacement, x component"), K::Derived, f,
                                      [er, ex](PointEvaluator& e) { return Eps0 * e.value(er) * e.value(ex); });
                const int dy = define(p + QStringLiteral("Dy"), dfield, tr("electric displacement, y component"), K::Derived, f,
                                      [er, ey](PointEvaluator& e) { return Eps0 * e.value(er) * e.value(ey); });
                define(p + QStringLiteral("normD"), dfield, tr("electric displacement, norm"), K::Derived, f,
                       [dx, dy](PointEvaluator& e) { return std::hypot(e.value(dx), e.value(dy)); });
                define(p + QStringLiteral("We"), Dim::of(-1, 1, -2), tr("electric energy density"), K::Derived, f,
                       [er, ne](PointEvaluator& e) {
                           const double n = e.value(ne);
                           return 0.5 * Eps0 * e.value(er) * n * n;
                       });
            } else {
                const int sg = define(p + QStringLiteral("sigma"), Dim::of(-3, -1, 3, 2), tr("electrical conductivity"), K::Coefficient, f);
                const int jx = define(p + QStringLiteral("Jx"), jfield, tr("current density, x component"), K::Derived, f,
                                      [sg, ex](PointEvaluator& e) { return e.value(sg) * e.value(ex); });
                const int jy = define(p + QStringLiteral("Jy"), jfield, tr("current density, y component"), K::Derived, f,
                                      [sg, ey](PointEvaluator& e) { return e.value(sg) * e.value(ey); });
                define(p + QStringLiteral("normJ"), jfield, tr("current density, norm"), K::Derived, f,
                       [jx, jy](PointEvaluator& e) { return std::hypot(e.value(jx), e.value(jy)); });
                define(p + QStringLiteral("Qrh"), power, tr("resistive (Joule) heating"), K::Derived, f,
                       [sg, ne](PointEvaluator& e) {
                           const double n = e.value(ne);
                           return e.value(sg) * n * n;
                       });
            }
        } else if (field.type == QLatin1String("heat")) {
            const int gx = define(p + QStringLiteral("gradTx"), gradT, tr("temperature gradient, x component"), K::GradX, f);
            const int gy = define(p + QStringLiteral("gradTy"), gradT, tr("temperature gradient, y component"), K::GradY, f);
            define(p + QStringLiteral("gradT"), gradT, tr("temperature gradient, norm"), K::GradNorm, f);
            const int k = define(p + QStringLiteral("k"), Dim::of(1, 1, -3, 0, -1), tr("thermal conductivity"), K::Coefficient, f);
            // (gradients' signs: Ex is -dV/dx; gradTx is +dT/dx)
            const int qx = define(p + QStringLiteral("qx"), heatFlux, tr("conductive heat flux, x component"), K::Derived, f,
                                  [k, gx](PointEvaluator& e) { return -e.value(k) * e.value(gx); });
            const int qy = define(p + QStringLiteral("qy"), heatFlux, tr("conductive heat flux, y component"), K::Derived, f,
                                  [k, gy](PointEvaluator& e) { return -e.value(k) * e.value(gy); });
            define(p + QStringLiteral("normq"), heatFlux, tr("conductive heat flux, norm"), K::Derived, f,
                   [qx, qy](PointEvaluator& e) { return std::hypot(e.value(qx), e.value(qy)); });
        }
    }
}

std::vector<FieldInfo> fieldInfos(const Model& model)
{
    std::vector<FieldInfo> out;
    for (const Node* p : model.physics()) {
        if (!p->enabled) continue;
        const bool heat = p->type == QLatin1String("heat");
        out.push_back({p->tag, p->type, heat ? QStringLiteral("T") : QStringLiteral("V"), heat ? Dim::temperature() : Dim::voltage()});
    }
    return out;
}

} // namespace

QList<Variable> modelVariables(const Model& model)
{
    QList<Variable> out;
    QSet<QString> names;
    declareVariables(fieldInfos(model), [&](const QString& name, const Dim& dim, const QString& description, Solution::VariableDef::Kind,
                                            int, std::function<double(PointEvaluator&)>) {
        if (!names.contains(name)) {
            names.insert(name);
            out << Variable{name, dim, description};
        }
        return int(out.size()) - 1;
    });
    return out;
}

void Solution::defineVariables()
{
    a_pointScope = std::make_unique<Scope>(&a_params->scope);
    a_defs.clear();
    std::vector<FieldInfo> fields;
    for (const Field& f : a_fields) fields.push_back({f.tag, f.type, f.variable, f.dim});
    declareVariables(fields, [this](const QString& name, const Dim& dim, const QString& description, VariableDef::Kind kind, int field,
                                    std::function<double(PointEvaluator&)> derived) {
        return define(name, dim, description, kind, field, std::move(derived));
    });
    a_globalScope = std::make_unique<Scope>(a_pointScope.get());
}

QList<Variable> Solution::variables() const
{
    QList<Variable> out;
    for (const VariableDef& d : a_defs) out << d.info;
    return out;
}

QList<Variable> Solution::globals() const
{
    return a_globals;
}

void Solution::setGlobal(const QString& name, double value, const Dim& dim, const QString& description)
{
    a_globalScope->setConstant(name, value, dim);
    for (Variable& v : a_globals)
        if (v.name == name) {
            v.dim = dim;
            v.description = description;
            return;
        }
    a_globals << Variable{name, dim, description};
}

std::optional<double> Solution::global(const QString& name) const
{
    for (const Variable& v : a_globals)
        if (v.name == name) return a_globalScope->constant(name)->value;
    return std::nullopt;
}

// ------------------------------------------------------------------ PointEvaluator

PointEvaluator::PointEvaluator(const Solution& solution) : a_s(solution)
{
    a_slots.assign(std::size_t(solution.pointScope().slotCount()), NaN);
    a_done.assign(a_slots.size(), 0);
    a_fields.resize(solution.fields().size());
}

void PointEvaluator::moveTo(int t, double xi, double eta)
{
    a_t = t;
    a_xi = xi;
    a_eta = eta;
    std::fill(a_done.begin(), a_done.end(), 0);
    for (FieldCache& c : a_fields) c.done = false;
    a_mapped = false;
}

QPointF PointEvaluator::point()
{
    if (!a_mapped) {
        // Where the finest space puts it (a quadratic one on an arc).
        const Space* best = nullptr;
        for (const Field& f : a_s.fields())
            if (!best || f.space->order() > best->order()) best = f.space.get();
        if (best) a_x = best->map(a_t, a_xi, a_eta).x;
        else {
            const auto& v = a_s.mesh().triangles[std::size_t(a_t)];
            const double s = a_s.mesh().unitScale;
            const QPointF p0 = a_s.mesh().nodes[std::size_t(v[0])] * s, p1 = a_s.mesh().nodes[std::size_t(v[1])] * s,
                          p2 = a_s.mesh().nodes[std::size_t(v[2])] * s;
            a_x = p0 + (p1 - p0) * a_xi + (p2 - p0) * a_eta;
        }
        a_mapped = true;
    }
    return a_x;
}

void PointEvaluator::field(int f, double& u, double& gx, double& gy)
{
    FieldCache& c = a_fields[std::size_t(f)];
    if (!c.done) {
        const Field& field = a_s.fields()[std::size_t(f)];
        const Space& sp = *field.space;
        const int d = a_s.mesh().domain[std::size_t(a_t)];
        if (field.solved && (d < 0 || !field.domains[std::size_t(d)])) {
            c.u = c.gx = c.gy = NaN;
        } else {
            Shape s;
            shapeAt(sp.order(), a_xi, a_eta, s);
            const Space::Map m = sp.map(a_t, a_xi, a_eta);
            const int* dofs = sp.dofs(a_t);
            double uu = 0, rx = 0, ry = 0;
            for (int k = 0; k < s.count; ++k) {
                const double v = field.values[std::size_t(dofs[k])];
                uu += s.n[k] * v;
                rx += s.dxi[k] * v;
                ry += s.deta[k] * v;
            }
            c.u = uu;
            Space::gradient(m, rx, ry, c.gx, c.gy);
        }
        c.done = true;
    }
    u = c.u;
    gx = c.gx;
    gy = c.gy;
}

double PointEvaluator::value(int slot)
{
    if (slot < 0 || slot >= int(a_slots.size())) return NaN;
    if (a_done[std::size_t(slot)]) return a_slots[std::size_t(slot)];
    const Solution::VariableDef& d = a_s.definitions()[std::size_t(slot)];
    double v = NaN;
    using K = Solution::VariableDef::Kind;
    switch (d.kind) {
    case K::X: v = point().x(); break;
    case K::Y: v = point().y(); break;
    case K::DomainNumber: v = a_s.mesh().domain[std::size_t(a_t)] + 1; break;
    case K::Size: {
        const auto& t = a_s.mesh().triangles[std::size_t(a_t)];
        const QPointF a = a_s.mesh().nodes[std::size_t(t[0])], b = a_s.mesh().nodes[std::size_t(t[1])], c = a_s.mesh().nodes[std::size_t(t[2])];
        const double area = std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2;
        v = std::sqrt(4 * area / std::sqrt(3.0)) * a_s.mesh().unitScale;
        break;
    }
    case K::Value: case K::GradX: case K::GradY: case K::GradNorm: {
        double u = 0, gx = 0, gy = 0;
        field(d.field, u, gx, gy);
        // The electric field is minus the potential's gradient.
        const bool electric = a_s.fields()[std::size_t(d.field)].type != QLatin1String("heat");
        const double sign = electric ? -1 : 1;
        v = d.kind == K::Value ? u : d.kind == K::GradX ? sign * gx : d.kind == K::GradY ? sign * gy : std::hypot(gx, gy);
        break;
    }
    case K::Coefficient: {
        const int dom = a_s.mesh().domain[std::size_t(a_t)];
        const auto& coefs = a_s.coefficients()[std::size_t(d.field)].c;
        if (dom >= 0 && dom < int(coefs.size()) && coefs[std::size_t(dom)].isValid() && a_depth < 8) {
            ++a_depth;
            v = eval(coefs[std::size_t(dom)]);
            --a_depth;
        }
        break;
    }
    case K::Derived:
        if (a_depth < 8) {
            ++a_depth;
            v = d.derived(*this);
            --a_depth;
        }
        break;
    }
    a_slots[std::size_t(slot)] = v;
    a_done[std::size_t(slot)] = 1;
    return v;
}

double PointEvaluator::eval(const Expression& e)
{
    if (e.isConstant()) return e.constant();
    for (int s : e.variables()) value(s);
    return e.eval(a_slots.data());
}

// ------------------------------------------------------------------ The problems

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

struct Terminal {
    QString name;
    bool floating = false; // a group of one potential
    Expression value;      // V0, or the charge (current) fed
    QVector<int> boundaries;
};

class Problem
{
public:
    Problem(Solution& solution, int field, Field& target, bool vacuum = false)
        : a_s(solution), a_fi(field), a_f(target), a_vacuum(vacuum)
    {
        for (const Node* p : solution.model().physics())
            if (p->tag == target.tag) a_node = p;
    }

    bool setup(QString* error, QStringList* warnings);
    /// Assembles and solves; the field's values set. With \a sweep its
    /// terminals' matrix too.
    bool solve(bool sweep, const QString& solver, double tolerance, QString* error);
    /// Whether its coefficients read field \a f.
    bool reads(int f) const { return a_reads.contains(f); }
    double totalSource() const { return a_totalSource; }
    /// u K u: twice the electric energy (es), the power dissipated (ec).
    double energy() const { return a_energy; }
    double sinkTemperature() const { return a_sink; }
    /// The terminal sweep's matrix entry for terminal \a name alone.
    double selfMatrix(const QString& name, QString* error);

private:
    void problem(const QString& what) { a_problems << what; }
    Expression compile(const QString& text, const Node& feature, const QString& key, bool spatial = true);
    void elementMatrices(bool withSources);
    bool buildEquations(QString* error);
    bool factorize(const QString& solver, double tolerance, QString* error);
    bool solveWith(const std::vector<double>& dirichlet, bool withSources, std::vector<double>& u, QString* error);
    std::vector<double> reactions(const std::vector<double>& u, bool withSources) const;

    Solution& a_s;
    int a_fi;
    Field& a_f;
    bool a_vacuum;
    const Node* a_node = nullptr;
    QStringList a_problems;
    QSet<int> a_reads;
    double a_scale = 1;                       // eps0 for electrostatics
    std::vector<Condition> a_conditions;      // by boundary
    std::vector<std::vector<Natural>> a_natural;   // by boundary
    std::vector<Terminal> a_terminals;
    double a_totalSource = 0;
    double a_energy = 0;
    double a_sink = NaN;

    // The equations.
    std::vector<int> a_eq;                    // by dof: its equation, -1 none, -3 Dirichlet
    std::vector<double> a_dirichlet;          // by dof
    std::vector<int> a_dofTerminal;           // by dof: a Dirichlet or group dof's terminal
    int a_neq = 0;
    std::vector<int> a_groupEq;               // by terminal (floating): its equation
    std::vector<double> a_K, a_F;             // element matrices, by element
    std::vector<double> a_KF0;                // their sources' part of F alone (for sweeps: none)
    struct EdgeTerm {
        int edge;
        double K[9], F[3];
        double made = 0;   // what its sources make (not a film's exchange)
    };
    std::vector<EdgeTerm> a_edges;
    Eigen::SparseMatrix<double> a_A;
    std::unique_ptr<Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>>> a_ldlt;
    std::unique_ptr<Eigen::SparseLU<Eigen::SparseMatrix<double>>> a_lu;
    std::unique_ptr<Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper,
                                             Eigen::IncompleteCholesky<double>>> a_cg;
};

Expression Problem::compile(const QString& text, const Node& feature, const QString& key, bool spatial)
{
    const Expression e = Expression::compile(text, a_s.pointScope());
    const QString where = tr("%1 of %2").arg(feature.kind() && feature.kind()->property(key) ? feature.kind()->property(key)->label : key,
                                              feature.name());
    if (!e.isValid()) {
        problem(tr("%1: %2").arg(where, e.error()));
        return e;
    }
    for (int slot : e.variables()) {
        const Solution::VariableDef& d = a_s.definitions()[std::size_t(slot)];
        if (d.field >= 0) a_reads.insert(d.field);
        if (!spatial && d.field >= 0) problem(tr("%1 may not read a field (%2) here").arg(where, d.info.name));
    }
    const PropertyDef* p = feature.kind() ? feature.kind()->property(key) : nullptr;
    if (p && !e.dim().isNone() && !(e.dim() == p->dim()))
        a_s.log << tr("Warning: %1 is in %2, where %3 is expected").arg(where, dimName(e.dim()), dimName(p->dim()));
    return e;
}

bool Problem::setup(QString* error, QStringList* warnings)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(a_f.tag);
        return false;
    }
    const Model& model = a_s.model();
    const Topology& topo = a_s.topology();
    const Mesh& mesh = a_s.mesh();
    const QString type = a_node->type;
    const bool es = type == QLatin1String("electrostatics"), ec = type == QLatin1String("currents");
    a_scale = es ? Eps0 : 1.0;
    QString why;
    // Its domains.
    const QVector<int> domains = resolveSelection(topo, model, Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    a_f.domains.assign(topo.domains.size(), 0);
    for (int d : domains) a_f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    // Each domain's material: the last that has it.
    std::vector<const Node*> material(topo.domains.size(), nullptr);
    for (const Node& m : model.materials().children) {
        if (!m.enabled) continue;
        const QVector<int> on = resolveSelection(topo, model, Level::Domain, m.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(m.name(), why));
        for (int d : on) material[std::size_t(d)] = &m;
    }
    // The domain features: the model's (last wins), the sources (they add).
    const QString modelFeature = es ? QStringLiteral("electrostatics/charge") : ec ? QStringLiteral("currents/conservation") : QStringLiteral("heat/solid");
    const QString property = es ? QStringLiteral("epsilonr") : ec ? QStringLiteral("sigma") : QStringLiteral("k");
    const QString propertyName = es ? tr("relative permittivity") : ec ? tr("electrical conductivity") : tr("thermal conductivity");
    std::vector<const Node*> modelOf(topo.domains.size(), nullptr);
    Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    coef.c.assign(topo.domains.size(), Expression());
    coef.sources.assign(topo.domains.size(), {});
    coef.sourceScale.assign(topo.domains.size(), 1.0);
    a_conditions.assign(topo.boundaries.size(), Condition());
    a_natural.assign(topo.boundaries.size(), {});
    a_terminals.clear();
    // The area of each domain (m²), for a total power spread over some.
    std::vector<double> area(topo.domains.size(), 0);
    for (std::size_t t = 0; t < mesh.triangles.size(); ++t) {
        const auto& v = mesh.triangles[t];
        const QPointF a = mesh.nodes[std::size_t(v[0])], b = mesh.nodes[std::size_t(v[1])], c = mesh.nodes[std::size_t(v[2])];
        area[std::size_t(mesh.domain[t])] += ((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2
                                             * mesh.unitScale * mesh.unitScale;
    }
    int order = 0;
    std::vector<int> exclusiveOrder(topo.boundaries.size(), -1);
    for (const Node& f : a_node->children) {
        ++order;
        if (!f.enabled) continue;
        const NodeKind* k = f.kind();
        if (!k) continue;
        const QVector<int> picked = resolveSelection(topo, model, k->selection, f.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(f.name(), why));
        if (k->selection == Level::Domain) {
            if (f.type == modelFeature) {
                for (int d : picked) modelOf[std::size_t(d)] = &f;
                continue;
            }
            // A source.
            Expression e;
            double scale = 1;
            if (f.type == QLatin1String("heat/source") && f.text(QStringLiteral("kind")) == QLatin1String("power")) {
                const std::optional<double> p = evaluateConstant(f.text(QStringLiteral("P0")), a_s.parameters().scope, Dim::power(), &why);
                if (!p) {
                    problem(tr("%1: %2").arg(f.name(), why));
                    continue;
                }
                double total = 0;
                for (int d : picked)
                    if (a_f.domains[std::size_t(d)]) total += area[std::size_t(d)];
                if (total <= 0) continue;
                e = Expression::compile(QStringLiteral("1"), a_s.pointScope());
                scale = *p / (total * a_s.thickness());
            } else {
                const QString key = es ? QStringLiteral("rho") : ec ? QStringLiteral("Qj") : QStringLiteral("Q0");
                e = compile(f.text(key), f, key);
            }
            if (!e.isValid()) continue;
            for (int d : picked) {
                if (!a_f.domains[std::size_t(d)]) continue;
                coef.sources[std::size_t(d)].push_back(e);
                coef.sourceScale[std::size_t(d)] = scale;   // (one total power a domain)
            }
            continue;
        }
        if (k->selection != Level::Boundary) continue;
        // Boundary features: those with a condition of their own (last wins),
        // and fluxes (they add).
        const QString t = f.type;
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
                c.value = compile(f.text(QStringLiteral("V0")), f, QStringLiteral("V0"), false);
            } else if (t == QLatin1String("heat/temperature")) {
                c.kind = Condition::Dirichlet;
                c.value = compile(f.text(QStringLiteral("T0")), f, QStringLiteral("T0"), false);
                if (c.value.isConstant()) a_sink = std::isnan(a_sink) ? c.value.constant() : std::min(a_sink, c.value.constant());
            } else if (t.endsWith(QLatin1String("/terminal")) || t.endsWith(QLatin1String("/floating"))) {
                Terminal term;
                term.name = t.endsWith(QLatin1String("/terminal")) ? f.text(QStringLiteral("name")) : f.name();
                term.floating = t.endsWith(QLatin1String("/floating"))
                                || (t == QLatin1String("currents/terminal") && f.text(QStringLiteral("drive")) == QLatin1String("current"));
                const QString key = !term.floating ? QStringLiteral("V0") : es ? QStringLiteral("Q0") : QStringLiteral("I0");
                term.value = compile(f.text(key), f, key, false);
                if (term.value.isValid() && !term.value.isConstant()) problem(tr("%1 of %2 must be a constant").arg(key, f.name()));
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
            for (int b : picked) {
                a_conditions[std::size_t(b)] = c;
                exclusiveOrder[std::size_t(b)] = order;
            }
            continue;
        }
        Natural n;
        if (t == QLatin1String("electrostatics/surfacecharge")) n.g = compile(f.text(QStringLiteral("rhos")), f, QStringLiteral("rhos"));
        else if (t == QLatin1String("currents/normalcurrent")) n.g = compile(f.text(QStringLiteral("Jn")), f, QStringLiteral("Jn"));
        else if (t == QLatin1String("heat/linesource")) n.g = compile(f.text(QStringLiteral("Qb")), f, QStringLiteral("Qb"));
        else if (t == QLatin1String("heat/flux")) {
            if (f.text(QStringLiteral("kind")) == QLatin1String("general")) {
                n.g = compile(f.text(QStringLiteral("q0")), f, QStringLiteral("q0"));
            } else {
                const Expression h = compile(f.text(QStringLiteral("h")), f, QStringLiteral("h"));
                const Expression text = compile(f.text(QStringLiteral("Text")), f, QStringLiteral("Text"));
                if (!h.isValid() || !text.isValid()) continue;
                n.q = h;
                n.made = false;
                n.g = Expression::compile(QStringLiteral("(") + f.text(QStringLiteral("h")) + QStringLiteral(")*(")
                                              + f.text(QStringLiteral("Text")) + QLatin1Char(')'),
                                          a_s.pointScope());
                if (text.isConstant()) a_sink = std::isnan(a_sink) ? text.constant() : std::min(a_sink, text.constant());
            }
        } else {
            continue;
        }
        if (!n.g.isValid() && !n.q.isValid()) continue;
        for (int b : picked) a_natural[std::size_t(b)].push_back(n);
    }
    // Each domain's coefficient: its feature's own, or its material's.
    for (int d = 0; d < int(topo.domains.size()); ++d) {
        if (!a_f.domains[std::size_t(d)]) continue;
        const Node* f = modelOf[std::size_t(d)];
        if (!f) {
            problem(tr("domain %1 has no %2 feature").arg(d + 1).arg(modelFeature.section(QLatin1Char('/'), 1)));
            continue;
        }
        QString text;
        if (a_vacuum) {
            text = QStringLiteral("1");
        } else if (f->text(QStringLiteral("source")) == QLatin1String("user")) {
            text = f->text(property);
        } else {
            const Node* m = material[std::size_t(d)];
            const QString object = topo.objectNames.value(topo.domains[std::size_t(d)].owner);
            if (!m) {
                problem(tr("domain %1 (%2) has no material: its %3 is needed").arg(d + 1).arg(object, propertyName));
                continue;
            }
            text = m->text(property);
            // Not given: its library entry's ({"material": "FR-4"} in a file).
            if (text.trimmed().isEmpty() && !m->text(QStringLiteral("library")).isEmpty())
                if (const std::optional<MaterialEntry> entry = findMaterial(m->text(QStringLiteral("library"))))
                    text = entry->properties.value(property).toString();
            if (text.trimmed().isEmpty()) {
                problem(tr("domain %1 (%2): its material, %3, has no %4").arg(d + 1).arg(object, m->name(), propertyName));
                continue;
            }
        }
        const Node& where = f->text(QStringLiteral("source")) == QLatin1String("user") || a_vacuum || !material[std::size_t(d)]
                                ? *f
                                : *material[std::size_t(d)];
        Expression e = compile(text, where, property);
        if (e.isValid() && e.isConstant() && !(e.constant() > 0) && !(es && e.constant() != 0))
            problem(tr("domain %1: its %2 must be more than 0").arg(d + 1).arg(propertyName));
        coef.c[std::size_t(d)] = e;
    }
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    Q_UNUSED(warnings);
    return true;
}

void Problem::elementMatrices(bool withSources)
{
    const Space& sp = *a_f.space;
    const Mesh& mesh = a_s.mesh();
    const Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    const int per = sp.perElement();
    const int nt = sp.elements();
    const double d = a_s.thickness();
    a_K.assign(std::size_t(nt) * std::size_t(per * per), 0);
    a_F.assign(std::size_t(nt) * std::size_t(per), 0);
    parallelFor(nt, [&](int begin, int end) {
        PointEvaluator ev(a_s);
        for (int t = begin; t < end; ++t) {
            const int dom = mesh.domain[std::size_t(t)];
            if (dom < 0 || !a_f.domains[std::size_t(dom)]) continue;
            const Expression& c = coef.c[std::size_t(dom)];
            const std::vector<Expression>& sources = coef.sources[std::size_t(dom)];
            const double sourceScale = coef.sourceScale[std::size_t(dom)];
            bool constant = c.isConstant();
            for (const Expression& s : sources) constant = constant && s.isConstant();
            const int degree = sp.curved(t) ? 5 : constant ? (sp.order() == 1 ? 1 : 2) : (sp.order() == 1 ? 2 : 4);
            const TriangleRule& rule = triangleRule(degree);
            double* K = &a_K[std::size_t(t) * std::size_t(per * per)];
            double* F = &a_F[std::size_t(t) * std::size_t(per)];
            Shape s;
            double gx[6], gy[6];
            for (const auto& q : rule.points) {
                shapeAt(sp.order(), q[0], q[1], s);
                const Space::Map m = sp.map(t, q[0], q[1]);
                const double w = q[2] * std::abs(m.det) * d;
                double cv = 0, fv = 0;
                if (constant) {
                    cv = c.constant();
                    for (const Expression& src : sources) fv += src.constant();
                } else {
                    ev.moveTo(t, q[0], q[1]);
                    cv = ev.eval(c);
                    for (const Expression& src : sources) fv += ev.eval(src);
                }
                cv *= a_scale;
                fv *= sourceScale;
                for (int i = 0; i < per; ++i) Space::gradient(m, s.dxi[i], s.deta[i], gx[i], gy[i]);
                for (int i = 0; i < per; ++i) {
                    for (int j = i; j < per; ++j) {
                        const double v = w * cv * (gx[i] * gx[j] + gy[i] * gy[j]);
                        K[i * per + j] += v;
                        if (j != i) K[j * per + i] += v;
                    }
                    if (withSources) F[i] += w * fv * s.n[i];
                }
            }
        }
    });
    // The boundaries' fluxes and Robin terms, edge by edge.
    a_edges.clear();
    PointEvaluator ev(a_s);
    const int lineN = sp.order() + 1;
    for (int e = 0; e < int(mesh.edges.size()); ++e) {
        const Mesh::Edge& me = mesh.edges[std::size_t(e)];
        const std::vector<Natural>& nat = a_natural[std::size_t(me.boundary)];
        if (nat.empty()) continue;
        // On a domain of this physics.
        int tri = -1;
        for (int side : {me.left, me.right})
            if (side >= 0 && a_f.domains[std::size_t(mesh.domain[std::size_t(side)])]) {
                tri = side;
                break;
            }
        if (tri < 0) continue;
        const std::array<int, 3>& tv = mesh.triangles[std::size_t(tri)];
        const int k = Space::localEdge(tv, me.a, me.b);
        const bool same = tv[std::size_t(k)] == me.a;
        EdgeTerm term{e, {}, {}};
        const int ne = sp.order() == 1 ? 2 : 3;
        for (const auto& q : lineRule(lineN)) {
            double jac = 0;
            sp.edgePoint(e, q[0], &jac);
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
            double g = 0, qq = 0, made = 0;
            for (const Natural& n : nat) {
                if (n.g.isValid()) {
                    const double v = ev.eval(n.g);
                    g += v;
                    if (n.made) made += v;
                }
                if (n.q.isValid()) qq += ev.eval(n.q);
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
            const double w = q[1] * jac * d;
            if (withSources) term.made += w * made;
            for (int i = 0; i < ne; ++i) {
                if (withSources) term.F[i] += w * g * N[i];
                for (int j = 0; j < ne; ++j) term.K[i * 3 + j] += w * qq * N[i] * N[j];
            }
        }
        a_edges.push_back(term);
    }
}

bool Problem::buildEquations(QString* error)
{
    const Space& sp = *a_f.space;
    const Mesh& mesh = a_s.mesh();
    const int nd = sp.size();
    const int per = sp.perElement();
    a_eq.assign(std::size_t(nd), -1);
    a_dirichlet.assign(std::size_t(nd), NaN);
    a_dofTerminal.assign(std::size_t(nd), -1);
    std::vector<int> group(std::size_t(nd), -1);
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = mesh.domain[std::size_t(t)];
        if (dom < 0 || !a_f.domains[std::size_t(dom)]) continue;
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
    bool anyFixed = false;
    for (int b : byOrder) {
        const Condition& c = a_conditions[std::size_t(b)];
        for (int e : edgesOf[std::size_t(b)]) {
            const std::array<int, 3> dofs = sp.edgeDofs(e);
            for (int dof : dofs) {
                if (dof < 0 || a_eq[std::size_t(dof)] == -1) continue;
                if (c.kind == Condition::Dirichlet) {
                    double v = NaN;
                    if (c.value.isConstant()) v = c.value.constant();
                    else if (c.value.isValid()) {
                        // Of x and y alone.
                        std::vector<double> values(std::size_t(a_s.pointScope().slotCount()), NaN);
                        values[std::size_t(a_s.pointScope().variable(QStringLiteral("x")))] = sp.point(dof).x();
                        values[std::size_t(a_s.pointScope().variable(QStringLiteral("y")))] = sp.point(dof).y();
                        v = c.value.eval(values.data());
                    }
                    a_eq[std::size_t(dof)] = -3;
                    a_dirichlet[std::size_t(dof)] = v;
                    group[std::size_t(dof)] = -1;
                    anyFixed = true;
                } else {
                    a_eq[std::size_t(dof)] = -4;
                    group[std::size_t(dof)] = c.group;
                    a_dirichlet[std::size_t(dof)] = NaN;
                }
                a_dofTerminal[std::size_t(dof)] = c.terminal;
            }
        }
    }
    bool robin = false;
    for (const auto& list : a_natural)
        for (const Natural& n : list) robin = robin || n.q.isValid();
    if (!anyFixed && !robin) {
        const QString type = a_node->type;
        *error = type == QLatin1String("heat")
                     ? tr("%1: no temperature is set and no heat leaves (no Temperature, no convective Heat Flux): the "
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
    // (a group none of whose dofs is left: drop it)
    if (a_neq == 0) {
        *error = tr("%1: nothing to solve - every value is set").arg(a_node->name());
        return false;
    }
    return true;
}

bool Problem::factorize(const QString& solver, double tolerance, QString* error)
{
    const Space& sp = *a_f.space;
    const int per = sp.perElement();
    // The matrix's pattern, row by row.
    std::vector<std::vector<int>> rows(static_cast<std::size_t>(a_neq));
    std::vector<int> local(6);
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = a_s.mesh().domain[std::size_t(t)];
        if (dom < 0 || !a_f.domains[std::size_t(dom)]) continue;
        local.clear();
        for (int i = 0; i < per; ++i) {
            const int q = a_eq[std::size_t(sp.dofs(t)[i])];
            if (q >= 0) local.push_back(q);
        }
        for (int i : local)
            for (int j : local) rows[std::size_t(i)].push_back(j);
    }
    std::vector<int> outer(std::size_t(a_neq) + 1, 0);
    for (int r = 0; r < a_neq; ++r) {
        std::vector<int>& row = rows[std::size_t(r)];
        std::sort(row.begin(), row.end());
        row.erase(std::unique(row.begin(), row.end()), row.end());
        if (row.empty() || !std::binary_search(row.begin(), row.end(), r)) row.insert(std::lower_bound(row.begin(), row.end(), r), r);
        outer[std::size_t(r) + 1] = outer[std::size_t(r)] + int(row.size());
    }
    a_A = Eigen::SparseMatrix<double>(a_neq, a_neq);
    a_A.resizeNonZeros(outer.back());
    std::copy(outer.begin(), outer.end(), a_A.outerIndexPtr());
    int* inner = a_A.innerIndexPtr();
    double* values = a_A.valuePtr();
    for (int r = 0; r < a_neq; ++r) {
        std::copy(rows[std::size_t(r)].begin(), rows[std::size_t(r)].end(), inner + outer[std::size_t(r)]);
        std::vector<int>().swap(rows[std::size_t(r)]);
    }
    std::fill(values, values + outer.back(), 0.0);
    auto add = [&](int i, int j, double v) {
        const int* begin = inner + outer[std::size_t(j)];
        const int* end = inner + outer[std::size_t(j) + 1];
        const int* at = std::lower_bound(begin, end, i);
        values[at - inner] += v;
    };
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = a_s.mesh().domain[std::size_t(t)];
        if (dom < 0 || !a_f.domains[std::size_t(dom)]) continue;
        const double* K = &a_K[std::size_t(t) * std::size_t(per * per)];
        const int* dofs = sp.dofs(t);
        for (int i = 0; i < per; ++i) {
            const int qi = a_eq[std::size_t(dofs[i])];
            if (qi < 0) continue;
            for (int j = 0; j < per; ++j) {
                const int qj = a_eq[std::size_t(dofs[j])];
                if (qj >= 0) add(qi, qj, K[i * per + j]);
            }
        }
    }
    for (const EdgeTerm& et : a_edges) {
        const std::array<int, 3> dofs = sp.edgeDofs(et.edge);
        const int ne = sp.order() == 1 ? 2 : 3;
        for (int i = 0; i < ne; ++i) {
            const int qi = a_eq[std::size_t(dofs[std::size_t(i)])];
            if (qi < 0) continue;
            for (int j = 0; j < ne; ++j) {
                const int qj = a_eq[std::size_t(dofs[std::size_t(j)])];
                if (qj >= 0) add(qi, qj, et.K[i * 3 + j]);
            }
        }
    }
    const bool iterative = solver == QLatin1String("iterative") || (solver != QLatin1String("direct") && a_neq > 400000);
    a_ldlt.reset();
    a_lu.reset();
    a_cg.reset();
    if (iterative) {
        a_cg = std::make_unique<std::remove_reference_t<decltype(*a_cg)>>();
        a_cg->setTolerance(std::min(1e-9, tolerance * 1e-3));
        a_cg->setMaxIterations(std::max(1000, 4 * int(std::sqrt(double(a_neq))) * 50));
        a_cg->compute(a_A);
        if (a_cg->info() == Eigen::Success) return true;
        a_s.log << tr("%1: the incomplete Cholesky factorization failed; the direct solver instead").arg(a_node->name());
        a_cg.reset();
    }
    a_ldlt = std::make_unique<Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>>>();
    a_ldlt->compute(a_A);
    if (a_ldlt->info() == Eigen::Success) {
        // A Cholesky of a matrix not positive definite: a pivot of 0 or less.
        const auto dvec = a_ldlt->vectorD();
        if ((dvec.array() > 0).all()) return true;
    }
    a_ldlt.reset();
    a_lu = std::make_unique<Eigen::SparseLU<Eigen::SparseMatrix<double>>>();
    a_lu->analyzePattern(a_A);
    a_lu->factorize(a_A);
    if (a_lu->info() != Eigen::Success) {
        *error = tr("%1: the matrix is singular - %2").arg(a_node->name(), QString::fromStdString(a_lu->lastErrorMessage()));
        a_lu.reset();
        return false;
    }
    return true;
}

bool Problem::solveWith(const std::vector<double>& dirichlet, bool withSources, std::vector<double>& u, QString* error)
{
    const Space& sp = *a_f.space;
    const int per = sp.perElement();
    Eigen::VectorXd rhs = Eigen::VectorXd::Zero(a_neq);
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = a_s.mesh().domain[std::size_t(t)];
        if (dom < 0 || !a_f.domains[std::size_t(dom)]) continue;
        const double* K = &a_K[std::size_t(t) * std::size_t(per * per)];
        const double* F = &a_F[std::size_t(t) * std::size_t(per)];
        const int* dofs = sp.dofs(t);
        for (int i = 0; i < per; ++i) {
            const int qi = a_eq[std::size_t(dofs[i])];
            if (qi < 0) continue;
            if (withSources) rhs[qi] += F[i];
            for (int j = 0; j < per; ++j)
                if (a_eq[std::size_t(dofs[j])] == -3) rhs[qi] -= K[i * per + j] * dirichlet[std::size_t(dofs[j])];
        }
    }
    for (const EdgeTerm& et : a_edges) {
        const std::array<int, 3> dofs = sp.edgeDofs(et.edge);
        const int ne = sp.order() == 1 ? 2 : 3;
        for (int i = 0; i < ne; ++i) {
            const int qi = a_eq[std::size_t(dofs[std::size_t(i)])];
            if (qi < 0) continue;
            if (withSources) rhs[qi] += et.F[i];
            for (int j = 0; j < ne; ++j)
                if (a_eq[std::size_t(dofs[std::size_t(j)])] == -3) rhs[qi] -= et.K[i * 3 + j] * dirichlet[std::size_t(dofs[std::size_t(j)])];
        }
    }
    if (withSources)
        for (int g = 0; g < int(a_terminals.size()); ++g)
            if (a_groupEq[std::size_t(g)] >= 0 && a_terminals[std::size_t(g)].value.isConstant())
                rhs[a_groupEq[std::size_t(g)]] += a_terminals[std::size_t(g)].value.constant();
    Eigen::VectorXd x;
    if (a_ldlt) x = a_ldlt->solve(rhs);
    else if (a_lu) x = a_lu->solve(rhs);
    else if (a_cg) {
        x = a_cg->solve(rhs);
        if (a_cg->info() != Eigen::Success) {
            *error = tr("%1: the conjugate gradients did not converge in %2 iterations (error %3): try the direct solver")
                         .arg(a_node->name())
                         .arg(a_cg->iterations())
                         .arg(a_cg->error());
            return false;
        }
    }
    if (!x.allFinite()) {
        *error = tr("%1: the solution is not finite").arg(a_node->name());
        return false;
    }
    u.assign(std::size_t(sp.size()), NaN);
    for (int dof = 0; dof < sp.size(); ++dof) {
        const int q = a_eq[std::size_t(dof)];
        if (q >= 0) u[std::size_t(dof)] = x[q];
        else if (q == -3) u[std::size_t(dof)] = dirichlet[std::size_t(dof)];
    }
    return true;
}

std::vector<double> Problem::reactions(const std::vector<double>& u, bool withSources) const
{
    // K u - F at the fixed degrees of freedom: what flows out there.
    const Space& sp = *a_f.space;
    const int per = sp.perElement();
    std::vector<double> r(std::size_t(sp.size()), 0);
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = a_s.mesh().domain[std::size_t(t)];
        if (dom < 0 || !a_f.domains[std::size_t(dom)]) continue;
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
    for (const EdgeTerm& et : a_edges) {
        const std::array<int, 3> dofs = sp.edgeDofs(et.edge);
        const int ne = sp.order() == 1 ? 2 : 3;
        for (int i = 0; i < ne; ++i) {
            if (a_eq[std::size_t(dofs[std::size_t(i)])] != -3) continue;
            double v = withSources ? -et.F[i] : 0;
            for (int j = 0; j < ne; ++j) v += et.K[i * 3 + j] * u[std::size_t(dofs[std::size_t(j)])];
            r[std::size_t(dofs[std::size_t(i)])] += v;
        }
    }
    return r;
}

bool Problem::solve(bool sweep, const QString& solver, double tolerance, QString* error)
{
    elementMatrices(true);
    if (!buildEquations(error)) return false;
    if (!factorize(solver, tolerance, error)) return false;
    std::vector<double> u;
    if (!solveWith(a_dirichlet, true, u, error)) return false;
    // What flows out of each terminal: its charge (current).
    const std::vector<double> r = reactions(u, true);
    a_f.terminals.clear();
    for (int g = 0; g < int(a_terminals.size()); ++g) {
        const Terminal& term = a_terminals[std::size_t(g)];
        Field::Terminal out;
        out.name = term.name;
        out.floating = term.floating;
        double charge = 0, potential = NaN;
        for (int dof = 0; dof < int(u.size()); ++dof) {
            if (a_dofTerminal[std::size_t(dof)] != g) continue;
            if (term.floating) potential = u[std::size_t(dof)];
            else {
                charge += r[std::size_t(dof)];
                potential = u[std::size_t(dof)];
            }
        }
        if (term.floating) charge = term.value.isConstant() ? term.value.constant() : NaN;
        out.charge = charge;
        out.potential = potential;
        a_f.terminals.push_back(out);
    }
    a_f.values = u;
    // Not solved: its initial value there (another physics may read it).
    for (double& v : a_f.values)
        if (std::isnan(v)) v = a_f.initial;
    a_f.solved = true;
    // Where it is not solved, NaN for the plots (PointEvaluator masks).
    // The terminal sweep: each voltage terminal at 1 V, the others and
    // every other fixed value at 0, no sources.
    a_f.matrix.clear();
    std::vector<int> voltage;
    for (int g = 0; g < int(a_terminals.size()); ++g)
        if (!a_terminals[std::size_t(g)].floating) voltage.push_back(g);
    if (sweep && !voltage.empty()) {
        const int n = int(voltage.size());
        a_f.matrix.assign(std::size_t(n), std::vector<double>(std::size_t(n), 0));
        for (int j = 0; j < n; ++j) {
            std::vector<double> dir(a_dirichlet.size(), 0);
            for (int dof = 0; dof < int(dir.size()); ++dof)
                if (a_eq[std::size_t(dof)] == -3) dir[std::size_t(dof)] = a_dofTerminal[std::size_t(dof)] == voltage[std::size_t(j)] ? 1 : 0;
            std::vector<double> uj;
            if (!solveWith(dir, false, uj, error)) return false;
            const std::vector<double> rj = reactions(uj, false);
            for (int i = 0; i < n; ++i) {
                double q = 0;
                for (int dof = 0; dof < int(rj.size()); ++dof)
                    if (a_dofTerminal[std::size_t(dof)] == voltage[std::size_t(i)] && a_eq[std::size_t(dof)] == -3) q += rj[std::size_t(dof)];
                a_f.matrix[std::size_t(i)][std::size_t(j)] = q;
            }
        }
        // Its terminals' names in the matrix's order.
        std::vector<Field::Terminal> ordered;
        for (int g : voltage) ordered.push_back(a_f.terminals[std::size_t(g)]);
        for (int g = 0; g < int(a_terminals.size()); ++g)
            if (a_terminals[std::size_t(g)].floating) ordered.push_back(a_f.terminals[std::size_t(g)]);
        a_f.terminals = ordered;
    }
    // The energy (es), the power (ec): u K u over the elements.
    double energy = 0;
    const Space& sp = *a_f.space;
    const int per = sp.perElement();
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = a_s.mesh().domain[std::size_t(t)];
        if (dom < 0 || !a_f.domains[std::size_t(dom)]) continue;
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
    return true;
}

double Problem::selfMatrix(const QString& name, QString* error)
{
    // Assembled without sources; one terminal at 1 V, every other fixed
    // value at 0; its charge.
    elementMatrices(false);
    if (!buildEquations(error)) return NaN;
    if (!factorize(QStringLiteral("direct"), 1e-9, error)) return NaN;
    int g = -1;
    for (int i = 0; i < int(a_terminals.size()); ++i)
        if (a_terminals[std::size_t(i)].name == name && !a_terminals[std::size_t(i)].floating) g = i;
    if (g < 0) {
        *error = tr("%1 has no terminal %2 at a voltage").arg(a_node->name(), name);
        return NaN;
    }
    std::vector<double> dir(a_dirichlet.size(), 0);
    for (int dof = 0; dof < int(dir.size()); ++dof)
        if (a_eq[std::size_t(dof)] == -3) dir[std::size_t(dof)] = a_dofTerminal[std::size_t(dof)] == g ? 1 : 0;
    std::vector<double> u;
    if (!solveWith(dir, false, u, error)) return NaN;
    const std::vector<double> r = reactions(u, false);
    double q = 0;
    for (int dof = 0; dof < int(r.size()); ++dof)
        if (a_dofTerminal[std::size_t(dof)] == g && a_eq[std::size_t(dof)] == -3) q += r[std::size_t(dof)];
    return q;
}

} // namespace

// ------------------------------------------------------------------ Studies

namespace {

QString matrixName(const QString& tag, const QString& letter, const QString& a, const QString& b)
{
    return tag + QLatin1Char('.') + letter + QLatin1Char('_') + a + QLatin1Char('_') + b;
}

void setGlobals(Solution& s, Field& f, double energy, double source, double sink)
{
    const QString p = f.tag + QLatin1Char('.');
    const bool es = f.type == QLatin1String("electrostatics"), ec = f.type == QLatin1String("currents");
    const Dim charge = es ? Dim::charge() : Dim::current();
    const QString q = es ? QStringLiteral("Q") : QStringLiteral("I");
    for (const Field::Terminal& t : f.terminals) {
        s.setGlobal(p + QStringLiteral("V_") + t.name, t.potential, Dim::voltage(), QCoreApplication::translate("qucs_s::fem::Solver", "terminal %1's potential").arg(t.name));
        s.setGlobal(p + q + QLatin1Char('_') + t.name, t.charge, charge,
                    es ? QCoreApplication::translate("qucs_s::fem::Solver", "terminal %1's charge").arg(t.name)
                       : QCoreApplication::translate("qucs_s::fem::Solver", "the current out of terminal %1").arg(t.name));
    }
    const int n = int(f.matrix.size());
    if (n > 0) {
        const QString letter = es ? QStringLiteral("C") : QStringLiteral("G");
        const Dim dim = es ? Dim::capacitance() : Dim::conductance();
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                const QString a = f.terminals[std::size_t(i)].name, b = f.terminals[std::size_t(j)].name;
                const double v = f.matrix[std::size_t(i)][std::size_t(j)];
                const QString what = es ? QCoreApplication::translate("qucs_s::fem::Solver", "capacitance matrix, %1 %2").arg(a, b)
                                        : QCoreApplication::translate("qucs_s::fem::Solver", "conductance matrix, %1 %2").arg(a, b);
                s.setGlobal(matrixName(f.tag, letter, a, b), v, dim, what);
                if (a.size() == 1 && b.size() == 1) s.setGlobal(p + letter + a + b, v, dim, what);
            }
        if (ec) {
            // The resistance of the first terminal to the others and the
            // grounds; the matrix's inverse.
            s.setGlobal(p + QStringLiteral("R"), 1.0 / f.matrix[0][0], Dim::resistance(),
                        QCoreApplication::translate("qucs_s::fem::Solver", "resistance from terminal %1 to the others and the grounds")
                            .arg(f.terminals[0].name));
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
                                    QCoreApplication::translate("qucs_s::fem::Solver", "resistance matrix, %1 %2").arg(a, b));
                        if (a.size() == 1 && b.size() == 1) s.setGlobal(p + QStringLiteral("R") + a + b, r(i, j), Dim::resistance());
                    }
            }
        }
    }
    if (es) s.setGlobal(p + QStringLiteral("W"), 0.5 * energy, Dim::of(2, 1, -2), QCoreApplication::translate("qucs_s::fem::Solver", "electric energy"));
    if (ec) s.setGlobal(p + QStringLiteral("P"), energy, Dim::power(), QCoreApplication::translate("qucs_s::fem::Solver", "power dissipated"));
    if (f.type == QLatin1String("heat")) {
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
        s.setGlobal(p + QStringLiteral("Tmax"), tmax, Dim::temperature(), QCoreApplication::translate("qucs_s::fem::Solver", "highest temperature"));
        s.setGlobal(p + QStringLiteral("Tmin"), tmin, Dim::temperature(), QCoreApplication::translate("qucs_s::fem::Solver", "lowest temperature"));
        s.setGlobal(p + QStringLiteral("P"), source, Dim::power(), QCoreApplication::translate("qucs_s::fem::Solver", "heat made by the sources"));
        if (!std::isnan(sink) && source != 0)
            s.setGlobal(p + QStringLiteral("Rth"), (tmax - sink) / source, Dim::of(-2, -1, 3, 0, 1),
                        QCoreApplication::translate("qucs_s::fem::Solver",
                                                    "thermal resistance: (Tmax - the coolest boundary temperature) / P"));
    }
}

} // namespace

StudyResult solveStudy(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Mesh> mesh,
                       const QString& studyTag, const Progress& progress)
{
    StudyResult out;
    QElapsedTimer timer;
    timer.start();
    if (!mesh || !mesh->topology) {
        out.error = tr("there is no mesh");
        return out;
    }
    const Node* study = nullptr;
    for (const Node* s : model.studies())
        if (studyTag.isEmpty() ? !study : s->tag == studyTag) study = s;
    if (!study) {
        out.error = studyTag.isEmpty() ? tr("the model has no study") : tr("there is no study %1").arg(studyTag);
        return out;
    }
    if (!parameters->errors.isEmpty()) {
        QStringList list;
        for (auto it = parameters->errors.cbegin(); it != parameters->errors.cend(); ++it) list << tr("%1: %2").arg(it.key(), it.value());
        out.error = tr("the parameters: %1").arg(list.join(QLatin1String("; ")));
        return out;
    }
    auto solution = std::make_shared<Solution>(model, parameters, mesh);
    solution->studyTag = study->tag;
    if (solution->fields().empty()) {
        out.error = tr("the model has no physics to solve");
        return out;
    }
    auto tell = [&](double f, const QString& what) { return !progress || progress(f, what); };
    const int steps = std::max<int>(1, int(study->children.size()));
    int stepIndex = 0;
    for (const Node& step : study->children) {
        if (!step.enabled) continue;
        if (step.type != QLatin1String("stationary")) {
            out.error = tr("%1: a %2 step is not solved yet").arg(study->name(), step.name());
            return out;
        }
        QStringList tags = step.list(QStringLiteral("physics"));
        if (tags.isEmpty())
            for (const Field& f : solution->fields()) tags << f.tag;
        std::vector<int> which;
        for (const QString& t : tags) {
            const int fi = solution->fieldIndex(t);
            if (fi < 0) {
                out.error = tr("%1: there is no physics %2 (enabled) to solve").arg(step.name(), t);
                return out;
            }
            which.push_back(fi);
        }
        const QString solver = step.text(QStringLiteral("solver"));
        const bool sweep = step.flag(QStringLiteral("sweep"));
        double tolerance = 1e-6;
        if (const std::optional<double> t = evaluateConstant(step.text(QStringLiteral("tolerance")), parameters->scope, Dim()); t && *t > 0)
            tolerance = *t;
        const int maxIter = std::clamp(step.integer(QStringLiteral("maxiter")), 1, 1000);
        std::vector<std::unique_ptr<Problem>> problems;
        QStringList errors;
        for (int fi : which) {
            auto p = std::make_unique<Problem>(*solution, fi, solution->fields()[std::size_t(fi)]);
            QString why;
            QStringList warnings;
            if (!p->setup(&why, &warnings)) errors << why;
            problems.push_back(std::move(p));
        }
        if (!errors.isEmpty()) {
            out.error = errors.join(QLatin1Char('\n'));
            return out;
        }
        // Coupled: a physics reads its own field or another solved here.
        bool coupled = false;
        for (std::size_t i = 0; i < problems.size(); ++i)
            for (int fj : which) coupled = coupled || problems[i]->reads(fj);
        const int iterations = coupled ? maxIter : 1;
        std::vector<double> energies(problems.size(), 0);
        bool converged = !coupled;
        for (int it = 0; it < iterations; ++it) {
            double change = 0;
            for (std::size_t i = 0; i < problems.size(); ++i) {
                Field& f = solution->fields()[std::size_t(which[i])];
                const double base = (stepIndex + double(it * problems.size() + i) / double(iterations * problems.size())) / steps;
                if (!tell(0.05 + 0.9 * base, tr("Solving %1").arg(f.tag))) {
                    out.error = tr("cancelled");
                    return out;
                }
                const std::vector<double> before = f.values;
                QString why;
                if (!problems[i]->solve(sweep && (!coupled || it + 1 == iterations), solver, tolerance, &why)) {
                    out.error = why;
                    return out;
                }
                double num = 0, den = 0;
                for (std::size_t k = 0; k < f.values.size(); ++k) {
                    const double d = f.values[k] - before[k];
                    num += d * d;
                    den += f.values[k] * f.values[k];
                }
                change = std::max(change, den > 0 ? std::sqrt(num / den) : std::sqrt(num));
            }
            if (coupled) {
                solution->log << tr("%1, iteration %2: relative change %3").arg(step.name()).arg(it + 1).arg(change, 0, 'g', 3);
                if (change < tolerance && it > 0) {
                    converged = true;
                    // Once more with the sweeps, when asked.
                    if (sweep)
                        for (auto& p : problems) {
                            QString why;
                            if (!p->solve(true, solver, tolerance, &why)) {
                                out.error = why;
                                return out;
                            }
                        }
                    break;
                }
            }
        }
        if (!converged) out.warnings << tr("%1: the coupled physics did not converge in %2 iterations").arg(step.name()).arg(maxIter);
        for (std::size_t i = 0; i < problems.size(); ++i) {
            Field& f = solution->fields()[std::size_t(which[i])];
            setGlobals(*solution, f, problems[i]->energy(), problems[i]->totalSource(), problems[i]->sinkTemperature());
        }
        ++stepIndex;
    }
    solution->seconds = timer.elapsed() / 1000.0;
    int dofs = 0;
    for (const Field& f : solution->fields())
        if (f.solved) dofs += f.space->size();
    solution->log << tr("Solved %1 degrees of freedom in %2 s").arg(dofs).arg(solution->seconds, 0, 'f', 2);
    tell(1.0, tr("Done"));
    out.solution = solution;
    return out;
}

double vacuumCapacitance(const Solution& solution, const QString& tag, const QString& terminal, QString* error)
{
    Solution& s = const_cast<Solution&>(solution);
    const int fi = s.fieldIndex(tag);
    if (fi < 0) {
        if (error) *error = tr("there is no physics %1").arg(tag);
        return NaN;
    }
    // A field of its own, on the same space; the coefficients put back after.
    Field scratch = s.fields()[std::size_t(fi)];
    const Solution::Coefficients saved = s.coefficients()[std::size_t(fi)];
    Problem p(s, fi, scratch, true);
    QString why;
    QStringList warnings;
    double c = NaN;
    if (p.setup(&why, &warnings)) c = p.selfMatrix(terminal, &why);
    s.coefficients()[std::size_t(fi)] = saved;
    if (std::isnan(c) && error) *error = why;
    return c;
}

} // namespace qucs_s::fem
