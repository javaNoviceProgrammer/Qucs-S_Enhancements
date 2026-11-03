/*
 * fem3_solver.cpp - the multiphysics solver in 3D: a solution's variables
 *                   and globals, its points read, the systems solved, and the
 *                   stationary study
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
#include "fem_results.h"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <limits>

namespace qucs_s::fem {

namespace detail3 {

QString tr(const char* text)
{
    return QCoreApplication::translate("Solver3", text);
}

} // namespace detail3

using detail3::tr;

namespace {

constexpr double Eps0 = 8.8541878188e-12;
const double NaN = std::numeric_limits<double>::quiet_NaN();

/// The physics solved in 3D, by type: their variable, unit and value before
/// they are solved.
struct FieldKind {
    const char* type;
    const char* variable;
    Dim dim;
    double initial;
};
const std::vector<FieldKind>& fieldKinds()
{
    static const std::vector<FieldKind> kinds = {{"heat", "T", Dim::temperature(), 293.15},
                                                 {"electrostatics", "V", Dim::voltage(), 0},
                                                 {"currents", "V", Dim::voltage(), 0}};
    return kinds;
}

const FieldKind* fieldKind(const QString& type)
{
    for (const FieldKind& k : fieldKinds())
        if (type == QLatin1String(k.type)) return &k;
    return nullptr;
}

} // namespace

// ------------------------------------------------------------------ Solution3

Solution3::Solution3(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Mesh3> mesh)
    : a_model(model), a_params(std::move(parameters)), a_mesh(std::move(mesh))
{
    a_pointScope = std::make_unique<Scope>(&a_params->scope);
    a_globalScope = std::make_unique<Scope>(a_pointScope.get());
}

int Solution3::fieldIndex(const QString& tag) const
{
    for (int i = 0; i < int(a_fields.size()); ++i)
        if (a_fields[std::size_t(i)].tag == tag) return i;
    return -1;
}

int Solution3::define(const QString& name, const Dim& dim, const QString& description, VariableDef::Kind kind, int field, int component,
                      double sign, std::function<double(PointEvaluator3&)> derived)
{
    if (a_pointScope->variable(name) >= 0) return a_pointScope->variable(name);
    // A parameter of a plain name (T, V, x...) keeps it: no such variable.
    if (!name.contains(QLatin1Char('.')) && a_params->scope.constant(name)) return -1;
    const int slot = a_pointScope->addVariable(name, dim);
    if (int(a_defs.size()) <= slot) a_defs.resize(std::size_t(slot) + 1);
    VariableDef& d = a_defs[std::size_t(slot)];
    d.info = {name, dim, description, QString(), QString()};
    d.kind = kind;
    d.field = field;
    d.component = component;
    d.sign = sign;
    d.derived = std::move(derived);
    return slot;
}

void Solution3::defineVariables()
{
    using K = VariableDef::Kind;
    define(QStringLiteral("x"), Dim::length(), tr("x coordinate"), K::X, -1);
    define(QStringLiteral("y"), Dim::length(), tr("y coordinate"), K::Y, -1);
    define(QStringLiteral("z"), Dim::length(), tr("z coordinate"), K::Z, -1);
    define(QStringLiteral("t"), Dim::of(0, 0, 1), tr("time"), K::Time, -1);
    define(QStringLiteral("dom"), Dim(), tr("the domain's number, from 1"), K::DomainNumber, -1);
    const Dim efield = Dim::voltage() / Dim::length(), dfield = Dim::charge() / Dim::area(), jfield = Dim::current() / Dim::area();
    const Dim power = Dim::power() / Dim::of(3), gradT = Dim::temperature() / Dim::length(), heatFlux = Dim::power() / Dim::area();
    const char* axis[3] = {"x", "y", "z"};
    for (int f = 0; f < int(a_fields.size()); ++f) {
        const Field3& field = a_fields[std::size_t(f)];
        const QString p = field.tag + QLatin1Char('.');
        define(p + field.variable, field.dim, tr("the dependent variable"), K::Value, f);
        define(field.variable, field.dim, tr("the dependent variable of %1").arg(field.tag), K::Value, f);
        // (A coefficient's tensor entry at a point, as the domain's
        // expression gives it.)
        auto entry = [f](PointEvaluator3& e, int k) {
            const Coefficients& c = e.solution().coefficients()[std::size_t(f)];
            const int d = e.domain();
            if (d < 0 || d >= int(c.c.size()) || !c.c[std::size_t(d)][std::size_t(k)].isValid()) return NaN;
            return e.eval(c.c[std::size_t(d)][std::size_t(k)]);
        };
        if (field.type == QLatin1String("electrostatics") || field.type == QLatin1String("currents")) {
            int e[3];
            for (int i = 0; i < 3; ++i)
                e[i] = define(p + QStringLiteral("E") + QString::fromLatin1(axis[i]), efield,
                              tr("electric field, %1 component").arg(QString::fromLatin1(axis[i])), K::Gradient, f, i, -1);
            define(p + QStringLiteral("normE"), efield, tr("electric field, norm"), K::GradNorm, f);
            const bool es = field.type == QLatin1String("electrostatics");
            const QString c = es ? QStringLiteral("epsr") : QStringLiteral("sigma");
            define(p + c, es ? Dim() : Dim::of(-3, -1, 3, 2),
                   es ? tr("relative permittivity (where it is a tensor, the mean of its diagonal)")
                      : tr("electrical conductivity (where it is a tensor, the mean of its diagonal)"),
                   K::Coefficient, f, -1);
            // D = ε0 εr E, J = σ E: the tensor times E.
            const double scale = es ? Eps0 : 1.0;
            int flux[3];
            for (int i = 0; i < 3; ++i) {
                const QString name = p + (es ? QStringLiteral("D") : QStringLiteral("J")) + QString::fromLatin1(axis[i]);
                const QString what = es ? tr("electric displacement, %1 component").arg(QString::fromLatin1(axis[i]))
                                        : tr("current density, %1 component").arg(QString::fromLatin1(axis[i]));
                flux[i] = define(name, es ? dfield : jfield, what, K::Derived, f, 0, 1, [entry, e, i, scale](PointEvaluator3& ev) {
                    double s = 0;
                    for (int j = 0; j < 3; ++j) s += entry(ev, 3 * i + j) * ev.value(e[j]);
                    return scale * s;
                });
            }
            define(p + (es ? QStringLiteral("normD") : QStringLiteral("normJ")), es ? dfield : jfield,
                   es ? tr("electric displacement, norm") : tr("current density, norm"), K::Derived, f, 0, 1, [flux](PointEvaluator3& ev) {
                       const double a = ev.value(flux[0]), b = ev.value(flux[1]), c = ev.value(flux[2]);
                       return std::sqrt(a * a + b * b + c * c);
                   });
            const auto dot = [flux, e](PointEvaluator3& ev) {
                double s = 0;
                for (int i = 0; i < 3; ++i) s += ev.value(flux[i]) * ev.value(e[i]);
                return s;
            };
            if (es)
                define(p + QStringLiteral("We"), Dim::of(-1, 1, -2), tr("electric energy density"), K::Derived, f, 0, 1,
                       [dot](PointEvaluator3& ev) { return 0.5 * dot(ev); });
            else
                define(p + QStringLiteral("Qrh"), power, tr("resistive (Joule) heating"), K::Derived, f, 0, 1, dot);
        } else if (field.type == QLatin1String("heat")) {
            int g[3];
            for (int i = 0; i < 3; ++i)
                g[i] = define(p + QStringLiteral("gradT") + QString::fromLatin1(axis[i]), gradT,
                              tr("temperature gradient, %1 component").arg(QString::fromLatin1(axis[i])), K::Gradient, f, i, 1);
            define(p + QStringLiteral("gradT"), gradT, tr("temperature gradient, norm"), K::GradNorm, f);
            define(p + QStringLiteral("k"), Dim::of(1, 1, -3, 0, -1), tr("thermal conductivity (where it is a tensor, the mean of its diagonal)"),
                   K::Coefficient, f, -1);
            int q[3];
            for (int i = 0; i < 3; ++i)
                q[i] = define(p + QStringLiteral("q") + QString::fromLatin1(axis[i]), heatFlux,
                              tr("conductive heat flux, %1 component").arg(QString::fromLatin1(axis[i])), K::Derived, f, 0, 1,
                              [entry, g, i](PointEvaluator3& ev) {
                                  double s = 0;
                                  for (int j = 0; j < 3; ++j) s += entry(ev, 3 * i + j) * ev.value(g[j]);
                                  return -s;
                              });
            define(p + QStringLiteral("normq"), heatFlux, tr("conductive heat flux, norm"), K::Derived, f, 0, 1, [q](PointEvaluator3& ev) {
                const double a = ev.value(q[0]), b = ev.value(q[1]), c = ev.value(q[2]);
                return std::sqrt(a * a + b * b + c * c);
            });
        }
    }
}

bool Solution3::varies(const Expression& e) const
{
    for (int slot : e.variables()) {
        const VariableDef& d = a_defs[std::size_t(slot)];
        if (d.field >= 0 || d.kind == VariableDef::Time) return true;
    }
    return false;
}

bool Solution3::reads(const Expression& e, int f) const
{
    for (int slot : e.variables())
        if (a_defs[std::size_t(slot)].field == f) return true;
    return false;
}

QList<Variable> Solution3::variables() const
{
    QList<Variable> out;
    for (const VariableDef& d : a_defs) out << d.info;
    return out;
}

QList<Variable> Solution3::globals() const
{
    QList<Variable> out;
    for (const QString& name : a_globalOrder) {
        const Global& g = a_globals.at(name);
        out << Variable{name, g.dim, g.description, QString(), QString()};
    }
    return out;
}

void Solution3::setGlobal(const QString& name, double value, const Dim& dim, const QString& description)
{
    if (!a_globals.count(name)) a_globalOrder << name;
    a_globals[name] = Global{value, dim, description};
    a_globalScope->setConstant(name, value, dim);
}

std::optional<double> Solution3::global(const QString& name) const
{
    auto it = a_globals.find(name);
    if (it == a_globals.end()) return std::nullopt;
    return it->second.value;
}

QString Solution3::globalText(const QString& name, int digits) const
{
    auto it = a_globals.find(name);
    if (it == a_globals.end()) return {};
    return formatQuantity(it->second.value, it->second.dim, digits);
}

// ------------------------------------------------------------ PointEvaluator3

PointEvaluator3::PointEvaluator3(const Solution3& solution) : a_s(solution)
{
    const std::size_t n = std::size_t(a_s.pointScope().slotCount());
    a_values.assign(n, NaN);
    a_known.assign(n, 0);
    a_scratch.assign(n, NaN);
}

PointEvaluator3::~PointEvaluator3() = default;

void PointEvaluator3::at(int e, double xi, double eta, double zeta)
{
    // The point: by the first field's space (its map - an order 2 one's
    // curved where the triangle is), else the prism's corners.
    const Mesh3& m = a_s.mesh();
    Vec3 x;
    if (!a_s.fields().empty() && a_s.fields().front().space) {
        x = a_s.fields().front().space->map(e, xi, eta, zeta).x;
    } else {
        PrismShape s;
        prismShapeAt(1, xi, eta, zeta, s);
        for (int k = 0; k < 6; ++k) {
            const Vec3& n = m.nodes[std::size_t(m.prisms[std::size_t(e)][std::size_t(k)])];
            x.x += s.n[k] * n.x * m.unitScale;
            x.y += s.n[k] * n.y * m.unitScale;
            x.z += s.n[k] * n.z * m.unitScale;
        }
    }
    at(e, xi, eta, zeta, x);
}

void PointEvaluator3::at(int e, double xi, double eta, double zeta, const Vec3& x)
{
    a_prism = e;
    a_xi = xi;
    a_eta = eta;
    a_zeta = zeta;
    a_domain = e >= 0 ? a_s.mesh().domain[std::size_t(e)] : -1;
    a_x = x;
    std::fill(a_known.begin(), a_known.end(), char(0));
}

bool PointEvaluator3::at(const Vec3& x)
{
    const Mesh3& m = a_s.mesh();
    if (!m.plan) return false;
    if (!a_locator) a_locator = std::make_unique<Locator>(*m.plan);
    const double s = m.unitScale;
    int t = -1;
    double xi = 0, eta = 0;
    if (!a_locator->locate(QPointF(x.x / s, x.y / s), &t, &xi, &eta)) return false;
    const double z = x.z / s;
    const std::vector<double>& h = m.heights;
    if (h.size() < 2 || z < h.front() - 1e-12 * (std::abs(h.back()) + 1) || z > h.back() + 1e-12 * (std::abs(h.back()) + 1)) return false;
    int j = int(std::upper_bound(h.begin(), h.end(), z) - h.begin()) - 1;
    j = std::clamp(j, 0, int(h.size()) - 2);
    // (On a height between two layers: the one below it holds it too.)
    int e = -1;
    for (int k : {j, j - 1, j + 1}) {
        if (k < 0 || k > int(h.size()) - 2 || z < h[std::size_t(k)] - 1e-12 || z > h[std::size_t(k) + 1] + 1e-12) continue;
        for (const Field3& f : a_s.fields())
            if (f.space) {
                e = f.space->prismAt(t, k);
                break;
            }
        if (e >= 0) {
            j = k;
            break;
        }
    }
    if (e < 0) return false;
    const double zeta = std::clamp((z - h[std::size_t(j)]) / (h[std::size_t(j) + 1] - h[std::size_t(j)]), 0.0, 1.0);
    at(e, xi, eta, zeta);
    return true;
}

bool PointEvaluator3::field(int f, double& u, double g[3])
{
    u = NaN;
    g[0] = g[1] = g[2] = NaN;
    if (f < 0 || f >= int(a_s.fields().size()) || a_prism < 0) return false;
    const Field3& field = a_s.fields()[std::size_t(f)];
    if (!field.space || a_domain < 0 || a_domain >= int(field.domains.size()) || !field.domains[std::size_t(a_domain)]
        || field.values.size() != std::size_t(field.space->size()))
        return false;
    const Space3& sp = *field.space;
    PrismShape s;
    prismShapeAt(sp.order(), a_xi, a_eta, a_zeta, s);
    const Space3::Map m = sp.map(a_prism, a_xi, a_eta, a_zeta);
    const int* d = sp.dofs(a_prism);
    u = 0;
    g[0] = g[1] = g[2] = 0;
    for (int i = 0; i < s.count; ++i) {
        const double v = field.values[std::size_t(d[i])];
        double gi[3];
        Space3::gradient(m, s.dxi[i], s.deta[i], s.dzeta[i], gi);
        u += s.n[i] * v;
        for (int k = 0; k < 3; ++k) g[k] += gi[k] * v;
    }
    return true;
}

double PointEvaluator3::value(int slot)
{
    if (slot < 0 || slot >= int(a_values.size())) return NaN;
    if (a_known[std::size_t(slot)] == 1) return a_values[std::size_t(slot)];
    if (a_known[std::size_t(slot)] == 2) return NaN;   // (it reads itself)
    a_known[std::size_t(slot)] = 2;
    const Solution3::VariableDef& d = a_s.definitions()[std::size_t(slot)];
    double v = NaN;
    using K = Solution3::VariableDef::Kind;
    switch (d.kind) {
    case K::X: v = a_x.x; break;
    case K::Y: v = a_x.y; break;
    case K::Z: v = a_x.z; break;
    case K::Time: v = a_s.time(); break;
    case K::DomainNumber: v = a_domain + 1; break;
    case K::Value:
    case K::Gradient:
    case K::GradNorm: {
        double u = NaN, g[3];
        if (field(d.field, u, g)) {
            if (d.kind == K::Value) v = u;
            else if (d.kind == K::Gradient) v = d.sign * g[std::clamp(d.component, 0, 2)];
            else v = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        }
        break;
    }
    case K::Coefficient: {
        const Solution3::Coefficients& c = a_s.coefficients()[std::size_t(d.field)];
        if (a_domain >= 0 && a_domain < int(c.c.size())) {
            const auto& t = c.c[std::size_t(a_domain)];
            if (d.component >= 0) {
                if (t[std::size_t(d.component)].isValid()) v = eval(t[std::size_t(d.component)]);
            } else if (t[0].isValid()) {
                v = c.isotropic[std::size_t(a_domain)] ? eval(t[0]) : (eval(t[0]) + eval(t[4]) + eval(t[8])) / 3;
            }
        }
        break;
    }
    case K::Derived:
        if (d.derived) v = d.derived(*this);
        break;
    }
    a_values[std::size_t(slot)] = v;
    a_known[std::size_t(slot)] = 1;
    return v;
}

double PointEvaluator3::eval(const Expression& e)
{
    if (!e.isValid()) return NaN;
    if (e.isConstant()) return e.constant();
    for (int slot : e.variables()) {
        const double v = value(slot);
        a_scratch[std::size_t(slot)] = v;
    }
    // (A value found while another was - a derived one's - may have written
    // its own slots: each is written again, the same, before it is read.)
    for (int slot : e.variables()) a_scratch[std::size_t(slot)] = a_values[std::size_t(slot)];
    return e.eval(a_scratch.data());
}

// ------------------------------------------------------------------ System3

namespace detail3 {

bool System3::factorize(const Eigen::SparseMatrix<double>& a, const QString& solver, int rhs, double tolerance, const QString& name,
                        QStringList* log, QString* error)
{
    a_amg.reset();
    a_chol.reset();
    a_n = int(a.rows());
    a_tolerance = std::min(tolerance > 0 ? tolerance : 1e-9, 1e-9);
    // (Where a factorization pays: more right-hand sides than about 6 + n /
    // 20,000 - multigrid's set-up and solves against the METIS-ordered
    // Cholesky's, measured: even at 6 for 28,830 unknowns, 9 for 130,050, 17
    // for 270,400.)
    const bool direct = solver == QLatin1String("direct")
                        || (solver != QLatin1String("iterative") && accelerateInBuild() && acceleratedCholesky() && rhs >= 6 + a_n / 20000
                            && a_n <= 300000);
    if (direct) {
        a_chol = std::make_unique<SparseCholesky>();
        a_chol->setOrdering(SparseCholesky::Ordering::Metis);
        a_chol->analyzePattern(a);
        a_chol->factorize(a);
        if (a_chol->info() == Eigen::Success) {
            if (log)
                *log << tr("%1: %2 unknowns, %3 Cholesky (METIS order)")
                            .arg(name)
                            .arg(a_n)
                            .arg(accelerateInBuild() && acceleratedCholesky() ? tr("Accelerate's") : tr("Eigen's"));
            return true;
        }
        // (Not positive definite, or singular to rounding: said.)
        if (error)
            *error = a_chol->singularToRounding() ? tr("%1: the system is singular to rounding - a value fixed nowhere (a potential left "
                                                       "floating, a temperature held nowhere)?")
                                                        .arg(name)
                                                  : tr("%1: the system could not be factorized (%2)").arg(name, QString::fromStdString(a_chol->lastErrorMessage()));
        a_chol.reset();
        return false;
    }
    a_amg = std::make_unique<AmgSolver>();
    a_amg->setTolerance(a_tolerance);
    QString why;
    if (!a_amg->compute(a, &why) && log) *log << tr("%1: %2 - the incomplete Cholesky's conjugate gradients instead").arg(name, why);
    return true;
}

bool System3::solve(const Eigen::VectorXd& b, Eigen::VectorXd& x, const QString& name, QStringList* log, QString* error)
{
    if (a_chol) {
        x = a_chol->solve(b);
        return x.allFinite();
    }
    if (!a_amg) {
        if (error) *error = tr("%1: no system to solve").arg(name);
        return false;
    }
    IterativeReport r;
    const bool ok = a_amg->solve(b, x, &r, error);
    if (log) {
        if (r.fallback)
            *log << tr("%1: %2 unknowns, multigrid did not converge - the incomplete Cholesky's conjugate gradients: %3 iterations, "
                       "residual %4")
                        .arg(name)
                        .arg(a_n)
                        .arg(r.iterations)
                        .arg(r.residual, 0, 'g', 2);
        else
            *log << tr("%1: %2 unknowns, conjugate gradients with algebraic multigrid: %3 iterations, residual %4")
                        .arg(name)
                        .arg(a_n)
                        .arg(r.iterations)
                        .arg(r.residual, 0, 'g', 2);
    }
    if (!ok && error && error->isEmpty()) *error = tr("%1: the iterative solve did not converge").arg(name);
    return ok;
}

// ------------------------------------------------------------------ Problem3

Problem3::Problem3(Solution3& solution, int field) : a_s(solution), a_fi(field)
{
    const QString tag = field >= 0 && field < int(solution.fields().size()) ? solution.fields()[std::size_t(field)].tag : QString();
    for (const Node* p : solution.model().physics())
        if (p->tag == tag) a_node = p;
}

Expression Problem3::compile(const QString& text, const Node& feature, const QString& key, bool spatial)
{
    const Expression e = Expression::compile(text, a_s.pointScope());
    const PropertyDef* p = feature.kind() ? feature.kind()->property(key) : nullptr;
    const QString where = tr("%1 of %2").arg(p ? p->label : key, feature.name());
    if (!e.isValid()) {
        problem(tr("%1: %2").arg(where, e.error()));
        return e;
    }
    for (int slot : e.variables()) {
        const Solution3::VariableDef& d = a_s.definitions()[std::size_t(slot)];
        if (d.field >= 0) a_reads.insert(d.field);
        if (!spatial && d.field >= 0) problem(tr("%1 may not read a field (%2) here").arg(where, d.info.name));
    }
    if (p && !e.dim().isNone() && !(e.dim() == p->dim())) {
        const QString said = tr("Warning: %1 is in %2, where %3 is expected").arg(where, dimName(e.dim()), dimName(p->dim()));
        if (!a_s.log.contains(said)) a_s.log << said;
    }
    return e;
}

std::vector<const Node*> Problem3::materials()
{
    const Topology3& topo = a_s.topology();
    std::vector<const Node*> material(topo.domains.size(), nullptr);
    QString why;
    for (const Node& m : a_s.model().materials().children) {
        if (!m.enabled) continue;
        const QVector<int> on = resolveSelection3(topo, a_s.model(), Level::Domain, m.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(m.name(), why));
        for (int d : on)
            if (d >= 0 && d < int(material.size())) material[std::size_t(d)] = &m;
    }
    return material;
}

} // namespace detail3

// -------------------------------------------------------------------- study

QStringList unsolvedPhysics3(const Model& model)
{
    QStringList out;
    for (const Node* p : model.physics())
        if (p->enabled && !fieldKind(p->type)) out << p->name();
    return out;
}

namespace {

/// The physics \a subset of the problems solved in turn till they agree
/// (the relative change of every field under the tolerance): once, where
/// none reads another's field.
bool solveTogether(std::vector<std::unique_ptr<detail3::Problem3>>& problems, const std::vector<int>& subset, const detail3::Options3& o,
                   Solution3& s, const QString& stepName, QString* error)
{
    bool coupled = false;
    for (int i : subset)
        for (int j : subset)
            if (i != j && problems[std::size_t(i)]->reads(j)) coupled = true;
    const int rounds = coupled ? o.maxIterations : 1;
    for (int it = 0; it < rounds; ++it) {
        double change = 0;
        for (int i : subset) {
            if (o.cancelled && o.cancelled()) {
                *error = tr("cancelled");
                return false;
            }
            Field3& f = s.fields()[std::size_t(i)];
            const std::vector<double> before = f.values;
            if (!problems[std::size_t(i)]->solve(o, error)) return false;
            double num = 0, den = 0;
            for (std::size_t k = 0; k < f.values.size() && k < before.size(); ++k) {
                const double d = f.values[k] - before[k];
                num += d * d;
                den += f.values[k] * f.values[k];
            }
            change = std::max(change, den > 0 ? std::sqrt(num / den) : std::sqrt(num));
        }
        if (!coupled) return true;
        s.log << tr("%1, iteration %2: relative change %3").arg(stepName).arg(it + 1).arg(change, 0, 'g', 3);
        if (change < o.tolerance && it > 0) return true;
    }
    *error = tr("%1: the coupled physics did not converge in %2 iterations").arg(stepName).arg(o.maxIterations);
    return false;
}

} // namespace

StudyResult3 solveStudy3(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Mesh3> mesh,
                         const QString& studyTag, const Progress& progress)
{
    StudyResult3 out;
    QElapsedTimer timer;
    timer.start();
    if (!mesh || !mesh->topology || mesh->prisms.empty()) {
        out.error = tr("there is no mesh to solve on");
        return out;
    }
    if (!parameters) parameters = evaluateParameters(model);
    if (const QStringList unsolved = unsolvedPhysics3(model); !unsolved.isEmpty()) {
        out.error = tr("%1: not solved in 3D yet - heat, electrostatics and electric currents are").arg(unsolved.join(QLatin1String(", ")));
        return out;
    }
    const Node* study = nullptr;
    for (const Node& c : model.root.children)
        if (c.type == QLatin1String("study") && (studyTag.isEmpty() || c.tag == studyTag)) {
            study = &c;
            break;
        }
    if (!study) {
        out.error = studyTag.isEmpty() ? tr("the model has no study") : tr("there is no study %1").arg(studyTag);
        return out;
    }
    auto s = std::make_shared<Solution3>(model, parameters, mesh);
    s->studyTag = study->tag;
    // Each physics a field, on a space of its order (one space an order).
    std::shared_ptr<const Space3> spaces[3];
    for (const Node* p : s->model().physics()) {
        if (!p->enabled) continue;
        const FieldKind* k = fieldKind(p->type);
        Field3 f;
        f.tag = p->tag;
        f.type = p->type;
        f.variable = QString::fromLatin1(k->variable);
        f.dim = k->dim;
        f.initial = k->initial;
        const int order = p->text(QStringLiteral("order")) == QLatin1String("2") ? 2 : 1;
        if (!spaces[order]) spaces[order] = std::make_shared<Space3>(mesh, order);
        f.space = spaces[order];
        f.values.assign(std::size_t(f.space->size()), f.initial);
        f.domains.assign(mesh->topology->domains.size(), 0);
        s->fields().push_back(std::move(f));
    }
    if (s->fields().empty()) {
        out.error = tr("the model has no physics to solve");
        return out;
    }
    s->coefficients().resize(s->fields().size());
    s->defineVariables();
    std::vector<std::unique_ptr<detail3::Problem3>> problems;
    QStringList errors;
    for (int i = 0; i < int(s->fields().size()); ++i) {
        problems.push_back(detail3::makeScalarProblem3(*s, i));
        QString why;
        if (!problems.back()->setup(&why)) errors << why;
    }
    if (!errors.isEmpty()) {
        out.error = errors.join(QLatin1Char('\n'));
        out.log = s->log;
        return out;
    }
    for (auto& p : problems) p->initialize();
    int steps = 0;
    const int stepCount = int(std::count_if(study->children.begin(), study->children.end(), [](const Node& n) { return n.enabled; }));
    for (const Node& step : study->children) {
        if (!step.enabled) continue;
        if (step.type != QLatin1String("stationary")) {
            out.error = tr("%1: a %2 step is not solved in 3D yet - a Stationary one is")
                            .arg(step.name(), step.kind() ? step.kind()->title : step.type);
            out.log = s->log;
            return out;
        }
        detail3::Options3 o;
        o.solver = step.text(QStringLiteral("solver"));
        if (const std::optional<double> t = evaluateConstant(step.text(QStringLiteral("tolerance")), parameters->scope, Dim()); t && *t > 0)
            o.tolerance = *t;
        o.maxIterations = std::clamp(step.integer(QStringLiteral("maxiter")), 1, 1000);
        o.sweep = step.flag(QStringLiteral("sweep"));
        const double from = double(steps) / stepCount, span = 1.0 / stepCount;
        o.cancelled = [progress, from, span, &step] { return progress && !progress(from + 0.5 * span, step.name()); };
        if (progress && !progress(from, step.name())) {
            out.error = tr("cancelled");
            return out;
        }
        // Its physics: those chosen, else all.
        const QStringList chosen = step.list(QStringLiteral("physics"));
        std::vector<int> subset;
        for (int i = 0; i < int(s->fields().size()); ++i)
            if (chosen.isEmpty() || chosen.contains(s->fields()[std::size_t(i)].tag)) subset.push_back(i);
        if (subset.empty()) {
            out.error = tr("%1 solves none of the model's physics (%2)").arg(step.name(), chosen.join(QLatin1String(", ")));
            return out;
        }
        QString why;
        if (!solveTogether(problems, subset, o, *s, step.name(), &why)) {
            out.error = why;
            out.log = s->log;
            return out;
        }
        for (int i : subset) s->fields()[std::size_t(i)].solved = true;
        ++steps;
    }
    if (steps == 0) {
        out.error = tr("%1 has no step to solve: add a Stationary step").arg(study->name());
        return out;
    }
    for (auto& p : problems)
        if (p->field().solved) p->setGlobals();
    s->seconds = timer.elapsed() / 1000.0;
    s->log << tr("Solved in %1 s").arg(s->seconds, 0, 'f', 2);
    out.solution = s;
    out.log = s->log;
    return out;
}

QList<Variable> modelVariables3(const Model& model)
{
    // (A solution of the model's fields, on no mesh: its variables as
    // declared.)
    auto params = evaluateParameters(model);
    Solution3 s(model, params, nullptr);
    for (const Node* p : model.physics()) {
        if (!p->enabled) continue;
        const FieldKind* k = fieldKind(p->type);
        if (!k) continue;
        Field3 f;
        f.tag = p->tag;
        f.type = p->type;
        f.variable = QString::fromLatin1(k->variable);
        f.dim = k->dim;
        s.fields().push_back(f);
    }
    s.coefficients().resize(s.fields().size());
    s.defineVariables();
    return s.variables();
}

} // namespace qucs_s::fem
