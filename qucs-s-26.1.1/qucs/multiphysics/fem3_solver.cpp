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

#include "fem3_mesh.h"
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

Solution3::Snapshot Solution3::snapshotNow() const
{
    Snapshot s;
    s.time = a_time;
    for (const Field3& f : a_fields) {
        s.values.push_back(f.values);
        s.terminals.push_back(f.terminals);
        s.matrices.push_back(f.matrix);
    }
    s.globals = a_globals;
    return s;
}

void Solution3::addSnapshot()
{
    a_snapshots.push_back(snapshotNow());
    a_selected = int(a_snapshots.size()) - 1;
}

void Solution3::replaceLastSnapshot()
{
    if (a_snapshots.empty()) return;
    a_snapshots.back() = snapshotNow();
    a_selected = int(a_snapshots.size()) - 1;
}

void Solution3::select(int i)
{
    if (a_snapshots.empty()) return;
    if (i < 0 || i >= int(a_snapshots.size())) i = int(a_snapshots.size()) - 1;
    if (i == a_selected) return;
    const Snapshot& s = a_snapshots[std::size_t(i)];
    for (std::size_t f = 0; f < a_fields.size() && f < s.values.size(); ++f) {
        a_fields[f].values = s.values[f];
        a_fields[f].terminals = s.terminals[f];
        a_fields[f].matrix = s.matrices[f];
    }
    for (const auto& [name, g] : s.globals) {
        a_globals[name] = g;
        a_globalScope->setConstant(name, g.value, g.dim);
    }
    a_time = s.time;
    a_selected = i;
}

int Solution3::snapshotAt(double t) const
{
    int best = -1;
    for (int i = 0; i < int(a_snapshots.size()); ++i)
        if (best < 0 || std::abs(a_snapshots[std::size_t(i)].time - t) < std::abs(a_snapshots[std::size_t(best)].time - t)) best = i;
    return best;
}

std::vector<double> Solution3::globalSeries(const QString& text, Dim* dim, QString* why) const
{
    // (Each snapshot's globals into the scope - its constants, the expression
    // compiled again at each -, those of the one the fields are of again
    // after.) At rest: the globals as they are, once.
    std::vector<double> out;
    const std::size_t n = std::max<std::size_t>(a_snapshots.size(), 1);
    for (std::size_t i = 0; i < n; ++i) {
        if (!a_snapshots.empty())
            for (const auto& [name, g] : a_snapshots[i].globals) a_globalScope->setConstant(name, g.value, g.dim);
        const Expression e = Expression::compile(text, *a_globalScope);
        if (!e.isValid() || !e.isConstant()) {
            if (why) *why = e.isValid() ? tr("%1 varies over the model: a global, or an integral as one").arg(text) : e.error();
            out.clear();
            break;
        }
        if (dim) *dim = e.dim();
        out.push_back(e.constant());
    }
    for (const auto& [name, g] : a_globals) a_globalScope->setConstant(name, g.value, g.dim);
    return out;
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
    if (f == a_perturbed) u += a_perturbation;
    return true;
}

void PointEvaluator3::perturb(int f, double du)
{
    a_perturbed = du != 0 ? f : -1;
    a_perturbation = du;
    std::fill(a_known.begin(), a_known.end(), char(0));
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
                        QStringList* log, QString* error, bool symmetric)
{
    a_amg.reset();
    a_chol.reset();
    a_lu.reset();
    a_symmetric = symmetric;
    a_n = int(a.rows());
    a_tolerance = std::min(tolerance > 0 ? tolerance : 1e-9, 1e-9);
    if (!symmetric) {
        // A Newton's matrix: an LU where direct is asked, else BiCGStab with
        // multigrid (one right-hand side a step: no factorization pays).
        if (solver == QLatin1String("direct")) {
            a_lu = std::make_unique<SparseLu<double>>();
            a_lu->compute(a);
            if (a_lu->info() == Eigen::Success) {
                if (log) *log << tr("%1: %2 unknowns, %3 LU (Newton's matrix)").arg(name).arg(a_n).arg(a_lu->accelerated() ? tr("Accelerate's") : tr("Eigen's"));
                return true;
            }
            if (error) *error = tr("%1: Newton's matrix could not be factorized (%2)").arg(name, QString::fromStdString(a_lu->lastErrorMessage()));
            a_lu.reset();
            return false;
        }
        a_amg = std::make_unique<AmgSolver>();
        a_amg->setTolerance(a_tolerance);
        a_amg->setNonsymmetric(true);
        QString why;
        if (!a_amg->compute(a, &why) && log) *log << tr("%1: %2 - the incomplete LU's BiCGStab instead").arg(name, why);
        return true;
    }
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
    if (a_lu) {
        x = a_lu->solve(b);
        return x.allFinite();
    }
    if (!a_amg) {
        if (error) *error = tr("%1: no system to solve").arg(name);
        return false;
    }
    IterativeReport r;
    const bool ok = a_amg->solve(b, x, &r, error);
    if (log) {
        if (!a_symmetric)
            *log << tr("%1: %2 unknowns, %3 (Newton's matrix): %4 iterations, residual %5")
                        .arg(name)
                        .arg(a_n)
                        .arg(r.fallback ? tr("multigrid did not converge - the incomplete LU's BiCGStab") : tr("BiCGStab with algebraic multigrid"))
                        .arg(r.iterations)
                        .arg(r.residual, 0, 'g', 2);
        else if (r.fallback)
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

using Problems3 = std::vector<std::unique_ptr<detail3::Problem3>>;

/// The physics \a subset of the problems solved in turn till they agree
/// (the relative change of every field under the tolerance): once, where
/// none reads another's field. Each solved with its options (\a optionsOf).
bool solveTogether(Problems3& problems, const std::vector<int>& subset, const std::function<detail3::Options3(int)>& optionsOf,
                   Solution3& s, const QString& stepName, QString* error)
{
    bool coupled = false;
    for (int i : subset)
        for (int j : subset)
            if (i != j && problems[std::size_t(i)]->reads(j)) coupled = true;
    const detail3::Options3 o = optionsOf(subset.front());
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
            if (!problems[std::size_t(i)]->solve(optionsOf(i), error)) return false;
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

/// A step's options from its node: its linear solver, its nonlinear
/// iterations' tolerance and most, and (a Stationary one's) its terminal
/// sweep.
detail3::Options3 stepOptions(const Node& step, const Scope& scope)
{
    detail3::Options3 o;
    o.solver = step.text(QStringLiteral("solver"));
    if (const QString n = step.text(QStringLiteral("nonlinear")); !n.isEmpty()) o.nonlinear = n;
    if (const std::optional<double> t = evaluateConstant(step.text(QStringLiteral("tolerance")), scope, Dim()); t && *t > 0) o.tolerance = *t;
    o.maxIterations = std::clamp(step.integer(QStringLiteral("maxiter")), 1, 1000);
    o.sweep = step.type == QLatin1String("stationary") && step.flag(QStringLiteral("sweep"));
    return o;
}

/// Each problem's globals that a step solved, or an earlier one.
void setSolvedGlobals(Problems3& problems, const std::vector<int>& subset)
{
    for (int i = 0; i < int(problems.size()); ++i)
        if (problems[std::size_t(i)]->field().solved || std::find(subset.begin(), subset.end(), i) != subset.end())
            problems[std::size_t(i)]->setGlobals();
}

/*!
 * A Time Dependent step, as the 2D solver's: its physics that change in
 * time (heat, its ρ Cp ∂T/∂t) by BDF of order 2 - backward Euler for its
 * first steps, or alone; the implicit midpoint rule on fixed steps -, the
 * others at rest at each step, from the fields as they are (their initial
 * values, or a step's before); adaptive steps by Milne's device (the
 * solution against a predictor of its order) to the relative tolerance of
 * how much the fields have changed; the fields and globals kept at each
 * output time, its first included. \a tell is told how far (0 to 1) and
 * cancels with false.
 */
bool transientStep3(Problems3& problems, Solution3& s, const Node& node, const detail3::Options3& base, const std::vector<int>& subset,
                    const std::function<bool(double, const QString&)>& tell, QStringList* warnings, QString* error)
{
    Dim dim;
    QString why;
    const std::optional<std::vector<double>> listed = valueList(node.text(QStringLiteral("times")), s.parameters().scope, &dim, &why);
    if (!listed) {
        *error = tr("%1: the output times: %2").arg(node.name(), why);
        return false;
    }
    std::vector<double> times = *listed;
    if (!dim.isNone() && !(dim == Dim::of(0, 0, 1))) {
        *error = tr("%1: the output times are in %2, not in seconds").arg(node.name(), dimName(dim));
        return false;
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    if (times.size() < 2) {
        *error = tr("%1: give two output times at least (a start and an end)").arg(node.name());
        return false;
    }
    const double t0 = times.front(), range = times.back() - t0;
    std::vector<int> stepped, quasi;
    for (int i : subset) (problems[std::size_t(i)]->hasMass() ? stepped : quasi).push_back(i);
    if (stepped.empty())
        *warnings << tr("%1: none of its physics changes in time (heat does): each output time is solved at rest").arg(node.name());
    double rtol = 1e-3;
    if (const std::optional<double> r = evaluateConstant(node.text(QStringLiteral("rtol")), s.parameters().scope, Dim()); r && *r > 0) rtol = *r;
    const bool euler = node.text(QStringLiteral("method")) == QLatin1String("euler");
    // The implicit midpoint rule (order 2, no damping): backward Euler over
    // half the step to v, then u(t + h) = 2v - u(t). It takes fixed steps.
    const bool midpoint = node.text(QStringLiteral("method")) == QLatin1String("midpoint");
    const bool fixed = node.text(QStringLiteral("stepping")) == QLatin1String("fixed");
    if (midpoint && !fixed) {
        *error = tr("%1: the implicit midpoint rule takes fixed steps: set Steps to Fixed (and its time step)").arg(node.name());
        return false;
    }
    double h = 0;
    if (!node.text(QStringLiteral("dt")).trimmed().isEmpty()) {
        const std::optional<double> d = evaluateConstant(node.text(QStringLiteral("dt")), s.parameters().scope, Dim::of(0, 0, 1), &why);
        if (!d || !(*d > 0)) {
            *error = tr("%1: the time step: %2").arg(node.name(), d ? tr("it must be more than 0") : why);
            return false;
        }
        h = *d;
    } else if (fixed) {
        *error = tr("%1: a fixed step needs its time step").arg(node.name());
        return false;
    } else {
        h = 1e-3 * (times[1] - times[0]);
    }
    const double fixedStep = h;
    const double hmin = 1e-12 * range;
    auto atRest = [&base](int) { return base; };
    // The start: the fields as they are; what is at rest solved with them.
    s.setTime(t0);
    if (!quasi.empty() && !solveTogether(problems, quasi, atRest, s, node.name(), error)) return false;
    for (int i : quasi) s.fields()[std::size_t(i)].solved = true;
    setSolvedGlobals(problems, subset);
    s.addSnapshot();
    s.timeDependent = true;
    // History of the stepped fields: (time, values) newest first.
    struct History {
        std::vector<double> t;
        std::vector<std::vector<double>> u;
    };
    std::vector<History> hist(problems.size());
    std::vector<std::vector<double>> u0(problems.size());
    for (int i : stepped) {
        hist[std::size_t(i)].t.push_back(t0);
        hist[std::size_t(i)].u.push_back(s.fields()[std::size_t(i)].values);
        u0[std::size_t(i)] = s.fields()[std::size_t(i)].values;
    }
    double t = t0, hPrev = 0;
    int accepted = 0, rejected = 0;
    std::size_t out = 1;
    std::vector<std::vector<double>> histories(problems.size());
    std::vector<double> masses(problems.size(), 0);
    const int maxSteps = 1000000;
    while (out < times.size()) {
        if (accepted + rejected > maxSteps) {
            *error = tr("%1: more than a million steps; stopped at t = %2").arg(node.name(), formatQuantity(t, Dim::of(0, 0, 1)));
            return false;
        }
        const double target = times[out];
        if (fixed) h = fixedStep;
        // To the output time exactly, no sliver left before it: what is left
        // of it taken in one step, or in two halves.
        if (t + h >= target - 1e-9 * range) h = target - t;
        else if (target - (t + h) < 0.2 * h) h = (target - t) / 2;
        const int order = euler || midpoint || accepted < 2 ? 1 : 2;
        const double omega = hPrev > 0 ? h / hPrev : 1;
        // (Backward Euler's step: the whole, or the midpoint rule's half.)
        const double hb = midpoint ? h / 2 : h;
        std::vector<std::vector<double>> saved;
        for (const Field3& f : s.fields()) saved.push_back(f.values);
        s.setTime(t + hb);
        for (int i : stepped) {
            const History& hi = hist[std::size_t(i)];
            const std::vector<double>& un = hi.u[0];
            std::vector<double>& hv = histories[std::size_t(i)];
            hv.resize(un.size());
            if (order == 1) {
                masses[std::size_t(i)] = 1 / hb;
                for (std::size_t k = 0; k < un.size(); ++k) hv[k] = un[k] / hb;
            } else {
                const std::vector<double>& um = hi.u[1];
                masses[std::size_t(i)] = (1 + 2 * omega) / ((1 + omega) * h);
                const double a1 = (1 + omega), a2 = omega * omega / (1 + omega);
                for (std::size_t k = 0; k < un.size(); ++k) hv[k] = (a1 * un[k] - a2 * um[k]) / h;
            }
        }
        QString failed;
        const bool ok = solveTogether(problems, subset, [&](int i) {
            detail3::Options3 o = base;
            if (problems[std::size_t(i)]->hasMass()) {
                o.mass = masses[std::size_t(i)];
                o.history = &histories[std::size_t(i)];
            }
            return o;
        }, s, node.name(), &failed);
        auto restore = [&] {
            for (std::size_t f = 0; f < saved.size(); ++f) s.fields()[f].values = saved[f];
            s.setTime(t);
        };
        if (!ok) {
            if (!fixed && h > 4 * hmin && failed.contains(QLatin1String("did not converge"))) {
                restore();
                ++rejected;
                h /= 4;
                continue;
            }
            *error = failed;
            return false;
        }
        if (midpoint) {
            // From the midpoint to the step's end: 2v - u(t); the physics at
            // rest solved there.
            for (int i : stepped) {
                std::vector<double>& v = s.fields()[std::size_t(i)].values;
                const std::vector<double>& un = hist[std::size_t(i)].u[0];
                for (std::size_t k = 0; k < v.size() && k < un.size(); ++k) v[k] = 2 * v[k] - un[k];
            }
            s.setTime(t + h);
            if (!quasi.empty() && !solveTogether(problems, quasi, atRest, s, node.name(), error)) return false;
        }
        // The error of the step: the solution against a predictor of its
        // order (Milne's device).
        double err = 0;
        if (!fixed && accepted >= 1) {
            for (int i : stepped) {
                const History& hi = hist[std::size_t(i)];
                const std::vector<double>& uc = s.fields()[std::size_t(i)].values;
                const double h1 = hi.t[0] - hi.t[1];
                double ratio = 0;
                std::vector<double> pred(uc.size());
                if (order == 1 || hi.u.size() < 3) {
                    // Linear through the last two; backward Euler's error
                    // h/(2h + h1) of the gap.
                    const double w = h / h1;
                    for (std::size_t k = 0; k < uc.size(); ++k) pred[k] = hi.u[0][k] + w * (hi.u[0][k] - hi.u[1][k]);
                    ratio = order == 1 ? h / (2 * h + h1) : 1.0 / 3;
                } else {
                    // Quadratic through the last three (Lagrange's at t + h).
                    const double ta = hi.t[0], tb = hi.t[1], tc = hi.t[2], x = t + h;
                    const double la = (x - tb) * (x - tc) / ((ta - tb) * (ta - tc));
                    const double lb = (x - ta) * (x - tc) / ((tb - ta) * (tb - tc));
                    const double lc = (x - ta) * (x - tb) / ((tc - ta) * (tc - tb));
                    for (std::size_t k = 0; k < uc.size(); ++k) pred[k] = la * hi.u[0][k] + lb * hi.u[1][k] + lc * hi.u[2][k];
                    // BDF2's error constant over the predictor's and its own.
                    const double h2 = hi.t[1] - hi.t[2];
                    const double cc = (1 + omega) * (1 + omega) / (6 * omega * (1 + 2 * omega));
                    const double cp = (h + h1) * (h + h1 + h2) / (6 * h * h);
                    ratio = cc / (cp + cc);
                }
                double gap = 0, change = 0, size = 0;
                const std::vector<double>& first = u0[std::size_t(i)];
                for (std::size_t k = 0; k < uc.size(); ++k) {
                    gap = std::max(gap, std::abs(uc[k] - pred[k]));
                    change = std::max(change, std::abs(uc[k] - first[k]));
                    size = std::max(size, std::abs(uc[k]));
                }
                const double scale = std::max(change, 1e-9 * size + 1e-300);
                err = std::max(err, ratio * gap / (rtol * scale));
            }
        }
        if (!fixed && err > 1 && h > 4 * hmin) {
            restore();
            ++rejected;
            h *= std::max(0.1, 0.9 * std::pow(err, -1.0 / (order + 1)));
            continue;
        }
        // Taken.
        for (int i : stepped) {
            History& hi = hist[std::size_t(i)];
            hi.t.insert(hi.t.begin(), t + h);
            hi.u.insert(hi.u.begin(), s.fields()[std::size_t(i)].values);
            if (hi.t.size() > 3) {
                hi.t.pop_back();
                hi.u.pop_back();
            }
        }
        t += h;
        hPrev = h;
        ++accepted;
        if (std::abs(t - target) <= 1e-9 * range) {
            t = target;
            s.setTime(t);
            for (int i : subset) s.fields()[std::size_t(i)].solved = true;
            setSolvedGlobals(problems, subset);
            s.addSnapshot();
            ++out;
        }
        if (!fixed) h *= err > 0 ? std::clamp(0.9 * std::pow(err, -1.0 / (order + 1)), 0.2, 2.0) : 2.0;
        if (h < hmin) {
            *error = tr("%1: the time step became too small at t = %2").arg(node.name(), formatQuantity(t, Dim::of(0, 0, 1)));
            return false;
        }
        if (!tell((t - t0) / range, tr("%1: t = %2").arg(node.name(), formatQuantity(t, Dim::of(0, 0, 1))))) {
            *error = tr("cancelled");
            return false;
        }
    }
    s.log << tr("%1: %2 steps taken, %3 taken again with a smaller one; %4 output times")
                 .arg(node.name())
                 .arg(accepted)
                 .arg(rejected)
                 .arg(times.size());
    return true;
}

} // namespace

StudyResult3 solveStudy3(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Mesh3> mesh,
                         const QString& studyTag, const Progress& progress, const SolveAsked3& asked)
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
    // What it will hold and take, each physics', said before it begins;
    // past the limit not solved, unless forced, and what would bring it
    // under said.
    {
        QString solver = asked.solver;
        if (solver.isEmpty())
            for (const Node& step : study->children)
                if (step.enabled && step.type != QLatin1String("sweep")) {
                    solver = step.text(QStringLiteral("solver"));
                    break;
                }
        qint64 nonzeros[3] = {-1, -1, -1};
        for (const Field3& f : s->fields()) {
            const int order = f.space->order();
            if (nonzeros[order] < 0) nonzeros[order] = patternNonzeros(*f.space);
            Estimate3 e = estimateOn(*f.space, nonzeros[order], solver);
            e.physics = f.tag;
            if (const Node* p = s->model().find(f.tag)) e.name = p->name();
            s->log << tr("Estimated: %1").arg(estimateText(e));
            out.estimates.push_back(e);
        }
        if (const QString past = pastTheLimit(out.estimates, asked.limit); !past.isEmpty()) {
            if (!asked.force) {
                out.error = past;
                out.log = s->log;
                return out;
            }
            s->log << tr("Solved past the limit, as asked (force): %1").arg(past);
        }
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
    // (A Parametric Sweep is the run's, not a step: runStudy3.)
    const int stepCount = int(std::count_if(study->children.begin(), study->children.end(),
                                            [](const Node& n) { return n.enabled && n.type != QLatin1String("sweep"); }));
    for (const Node& step : study->children) {
        if (!step.enabled || step.type == QLatin1String("sweep")) continue;
        const bool inTime = step.type == QLatin1String("transient");
        if (step.type != QLatin1String("stationary") && !inTime) {
            out.error = tr("%1: a %2 step is not solved in 3D yet - a Stationary or a Time Dependent one is")
                            .arg(step.name(), step.kind() ? step.kind()->title : step.type);
            out.log = s->log;
            return out;
        }
        detail3::Options3 o = stepOptions(step, parameters->scope);
        if (!asked.solver.isEmpty()) o.solver = asked.solver;
        const double from = double(steps) / stepCount, span = 1.0 / stepCount;
        // (How far it is, as last told: what a solve's check of Cancel says.)
        auto at = std::make_shared<std::pair<double, QString>>(from + 0.5 * span, step.name());
        o.cancelled = [progress, at] { return progress && !progress(at->first, at->second); };
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
        if (inTime) {
            auto tell = [progress, at, from, span](double f, const QString& what) {
                *at = {from + span * std::clamp(f, 0.0, 1.0), what};
                return !progress || progress(at->first, at->second);
            };
            if (!transientStep3(problems, *s, step, o, subset, tell, &out.warnings, &why)) {
                out.error = why;
                out.log = s->log;
                return out;
            }
        } else {
            if (!solveTogether(problems, subset, [&o](int) { return o; }, *s, step.name(), &why)) {
                out.error = why;
                out.log = s->log;
                return out;
            }
            // (After a step in time: its last output time the fields at rest.)
            if (s->snapshotCount() > 0) {
                for (int i : subset) s->fields()[std::size_t(i)].solved = true;
                setSolvedGlobals(problems, subset);
                s->replaceLastSnapshot();
            }
        }
        for (int i : subset) s->fields()[std::size_t(i)].solved = true;
        ++steps;
    }
    if (steps == 0) {
        out.error = tr("%1 has no step to solve: add a Stationary step").arg(study->name());
        return out;
    }
    // The globals: those of the last output time, kept, or of the fields at
    // rest.
    if (s->snapshotCount() > 0) s->select(s->snapshotCount() - 1);
    else
        for (auto& p : problems)
            if (p->field().solved) p->setGlobals();
    s->seconds = timer.elapsed() / 1000.0;
    s->log << tr("Solved in %1 s").arg(s->seconds, 0, 'f', 2);
    out.solution = s;
    out.log = s->log;
    return out;
}

StudyRun3 runStudy3(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Topology3> topology,
                    std::shared_ptr<const Mesh3> mesh, const QString& studyTag, const Progress& progress, const SolveAsked3& asked)
{
    StudyRun3 run;
    run.mesh = mesh;
    if (!parameters) parameters = evaluateParameters(model);
    const Node* study = nullptr;
    for (const Node& c : model.root.children)
        if (c.type == QLatin1String("study") && (studyTag.isEmpty() || c.tag == studyTag)) {
            study = &c;
            break;
        }
    if (!study) {
        run.error = studyTag.isEmpty() ? tr("the model has no study") : tr("there is no study %1").arg(studyTag);
        return run;
    }
    const Node* sweep = nullptr;
    for (const Node& c : study->children)
        if (c.type == QLatin1String("sweep") && c.enabled) sweep = &c;
    auto meshOf = [&model, &run](const ParameterScope& ps, std::shared_ptr<const Topology3> topo, const Progress& p,
                                 std::shared_ptr<const Mesh3>* out) {
        if (!topo) {
            const GeometryBuild3 g = buildGeometry3(model, ps);
            if (!g.ok()) {
                const Node* n = model.find(g.failedAt);
                run.error = tr("the geometry: %1: %2").arg(n ? n->name() : g.failedAt, g.errors.value(g.failedAt));
                return false;
            }
            topo = g.topology;
        }
        const Mesh3Build m = buildMesh3(model, ps, topo, p);
        if (!m.mesh) {
            run.error = tr("the mesh: %1").arg(m.error);
            return false;
        }
        for (const QString& w : m.warnings)
            if (!run.warnings.contains(w)) run.warnings << w;
        *out = m.mesh;
        return true;
    };
    if (!sweep) {
        const Progress meshing = [&progress](double f, const QString& w) { return !progress || progress(0.3 * f, w); };
        if (!run.mesh && !meshOf(*parameters, topology, meshing, &run.mesh)) return run;
        const double from = mesh ? 0 : 0.3;
        StudyResult3 r = solveStudy3(
            model, parameters, run.mesh, study->tag, [&progress, from](double f, const QString& w) { return !progress || progress(from + (1 - from) * f, w); },
            asked);
        run.warnings << r.warnings;
        run.log = r.log;
        run.estimates = r.estimates;
        if (!r.solution) {
            run.error = r.error;
            return run;
        }
        run.solutions.push_back(r.solution);
        return run;
    }
    const std::optional<SweepPlan> plan = sweepPlan(model, *sweep, *parameters, &run.error);
    if (!plan) return run;
    run.swept = plan->swept;
    const std::size_t n = plan->points.size();
    // A point that finds no solution - cancelled, a step that fails, its
    // geometry or mesh not made -: after the first, the sweep stops there,
    // its points before kept and where and why said.
    auto stopAt = [&run, n, sweep](std::size_t k, const QString& where, const QString& why) {
        if (run.solutions.empty()) {
            run.error = tr("%1: %2").arg(where, why);
            return false;
        }
        const QString kept = run.solutions.size() == 1 ? tr("its point before kept") : tr("its %1 points before kept").arg(run.solutions.size());
        run.stopped = tr("%1 stopped at %2 (point %3 of %4): %5 - %6").arg(sweep->name(), where).arg(k + 1).arg(n).arg(why, kept);
        run.error.clear();
        return true;
    };
    std::shared_ptr<const Mesh3> base = mesh;
    for (std::size_t k = 0; k < n; ++k) {
        const QString where = plan->labels.at(int(k));
        const std::shared_ptr<ParameterScope> ps = evaluateParameters(model, plan->overrides[k]);
        if (!ps->errors.isEmpty()) {
            QStringList list;
            for (auto it = ps->errors.cbegin(); it != ps->errors.cend(); ++it) list << tr("%1: %2").arg(it.key(), it.value());
            if (stopAt(k, where, tr("the parameters: %1").arg(list.join(QLatin1String("; "))))) break;
            return run;
        }
        // The parameters it changes, and whether they shape the geometry.
        QSet<QString> changed;
        for (auto it = ps->values.cbegin(); it != ps->values.cend(); ++it)
            if (!parameters->values.contains(it.key()) || parameters->values.value(it.key()) != it.value()) changed.insert(it.key());
        const double from = double(k) / double(n), span = 1.0 / double(n);
        const QString told = tr("%1 (point %2 of %3)").arg(where).arg(k + 1).arg(n);
        auto sub = [&progress, from, span, told](double a, double b) {
            return Progress([&progress, from, span, told, a, b](double f, const QString& w) {
                return !progress || progress(from + span * (a + (b - a) * f), QStringLiteral("%1: %2").arg(told, w));
            });
        };
        std::shared_ptr<const Mesh3> m;
        if (plan->reshapes(changed)) {
            if (!meshOf(*ps, nullptr, sub(0, 0.3), &m)) {
                if (stopAt(k, where, run.error)) break;
                return run;
            }
        } else {
            if (!base && !meshOf(*parameters, topology, sub(0, 0.3), &base)) return run;
            run.mesh = base;
            m = base;
        }
        StudyResult3 r = solveStudy3(model, ps, m, study->tag, sub(0.3, 1), asked);
        if (k == 0) run.estimates = r.estimates;
        for (const QString& w : r.warnings)
            if (!run.warnings.contains(w)) run.warnings << w;
        if (!r.solution) {
            if (stopAt(k, where, r.error)) break;
            return run;
        }
        r.solution->label = where;
        r.solution->sweptValues = plan->values[k];
        run.log << QStringLiteral("%1: %2").arg(where, r.log.join(QLatin1String("; ")));
        run.solutions.push_back(r.solution);
    }
    if (!run.mesh) run.mesh = base;
    return run;
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
