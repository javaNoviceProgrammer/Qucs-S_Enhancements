/*
 * fem_solver.cpp - the multiphysics solver: solutions, their variables and
 *                  snapshots, the sparse systems, studies in steps (at
 *                  rest, in time) and their sweeps
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
#include "fem_problem.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <thread>

namespace qucs_s::fem {

namespace detail {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Solver", text);
}

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

// ------------------------------------------------------------------ LinearSystem

void LinearSystem::setPattern(int neq, const std::function<void(const std::function<void(const int*, int)>&)>& elements)
{
    a_n = neq;
    std::vector<std::vector<int>> rows(static_cast<std::size_t>(neq));
    elements([&](const int* eqs, int n) {
        for (int i = 0; i < n; ++i) {
            if (eqs[i] < 0) continue;
            std::vector<int>& row = rows[std::size_t(eqs[i])];
            for (int j = 0; j < n; ++j)
                if (eqs[j] >= 0) row.push_back(eqs[j]);
        }
    });
    std::vector<int> outer(std::size_t(neq) + 1, 0);
    for (int r = 0; r < neq; ++r) {
        std::vector<int>& row = rows[std::size_t(r)];
        std::sort(row.begin(), row.end());
        row.erase(std::unique(row.begin(), row.end()), row.end());
        if (!std::binary_search(row.begin(), row.end(), r)) row.insert(std::lower_bound(row.begin(), row.end(), r), r);
        outer[std::size_t(r) + 1] = outer[std::size_t(r)] + int(row.size());
    }
    // (Its pattern is symmetric: column j holds row j's columns.)
    a_A = Eigen::SparseMatrix<double>(neq, neq);
    a_A.resizeNonZeros(outer.back());
    std::copy(outer.begin(), outer.end(), a_A.outerIndexPtr());
    int* inner = a_A.innerIndexPtr();
    for (int r = 0; r < neq; ++r) {
        std::copy(rows[std::size_t(r)].begin(), rows[std::size_t(r)].end(), inner + outer[std::size_t(r)]);
        std::vector<int>().swap(rows[std::size_t(r)]);
    }
    clear();
    a_ldlt.reset();
    a_lu.reset();
    a_cg.reset();
    a_ldltAnalyzed = a_luAnalyzed = false;
}

void LinearSystem::clear()
{
    std::fill(a_A.valuePtr(), a_A.valuePtr() + a_A.nonZeros(), 0.0);
}

void LinearSystem::add(int i, int j, double v)
{
    const int* inner = a_A.innerIndexPtr();
    const int* outer = a_A.outerIndexPtr();
    const int* begin = inner + outer[j];
    const int* end = inner + outer[j + 1];
    const int* at = std::lower_bound(begin, end, i);
    a_A.valuePtr()[at - inner] += v;
}

bool LinearSystem::factorize(bool symmetric, const QString& solver, double tolerance, const QString& name, QStringList* log, QString* error)
{
    const bool iterative = symmetric && (solver == QLatin1String("iterative") || (solver != QLatin1String("direct") && a_n > 400000));
    a_cg.reset();
    if (iterative) {
        a_ldlt.reset();
        a_lu.reset();
        a_cg = std::make_unique<std::remove_reference_t<decltype(*a_cg)>>();
        a_cg->setTolerance(std::min(1e-9, tolerance * 1e-3));
        a_cg->setMaxIterations(std::max(1000, 4 * int(std::sqrt(double(a_n))) * 50));
        a_cg->compute(a_A);
        if (a_cg->info() == Eigen::Success) return true;
        if (log) *log << tr("%1: the incomplete Cholesky factorization failed; the direct solver instead").arg(name);
        a_cg.reset();
    }
    if (symmetric) {
        if (!a_ldlt) {
            a_ldlt = std::make_unique<Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>>>();
            a_ldltAnalyzed = false;
        }
        if (!a_ldltAnalyzed) {
            a_ldlt->analyzePattern(a_A);
            a_ldltAnalyzed = true;
        }
        a_ldlt->factorize(a_A);
        if (a_ldlt->info() == Eigen::Success) {
            // A Cholesky of a matrix not positive definite: a pivot of 0 or less.
            const auto dvec = a_ldlt->vectorD();
            if ((dvec.array() > 0).all()) {
                a_lu.reset();
                return true;
            }
        }
        a_ldlt.reset();
        a_ldltAnalyzed = false;
    } else {
        a_ldlt.reset();
        a_ldltAnalyzed = false;
    }
    if (!a_lu) {
        a_lu = std::make_unique<Eigen::SparseLU<Eigen::SparseMatrix<double>>>();
        a_luAnalyzed = false;
    }
    if (!a_luAnalyzed) {
        a_lu->analyzePattern(a_A);
        a_luAnalyzed = true;
    }
    a_lu->factorize(a_A);
    if (a_lu->info() != Eigen::Success) {
        *error = tr("%1: the matrix is singular - %2").arg(name, QString::fromStdString(a_lu->lastErrorMessage()));
        a_lu.reset();
        a_luAnalyzed = false;
        return false;
    }
    return true;
}

bool LinearSystem::solve(const Eigen::VectorXd& rhs, Eigen::VectorXd& x, const QString& name, QString* error) const
{
    if (a_ldlt) x = a_ldlt->solve(rhs);
    else if (a_lu) x = a_lu->solve(rhs);
    else if (a_cg) {
        x = a_cg->solve(rhs);
        if (a_cg->info() != Eigen::Success) {
            *error = tr("%1: the conjugate gradients did not converge in %2 iterations (error %3): try the direct solver")
                         .arg(name)
                         .arg(a_cg->iterations())
                         .arg(a_cg->error());
            return false;
        }
    } else {
        *error = tr("%1: nothing is factorized").arg(name);
        return false;
    }
    if (!x.allFinite()) {
        *error = tr("%1: the solution is not finite").arg(name);
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ Problem

Problem::Problem(Solution& solution, int field) : a_s(solution), a_fi(field)
{
    const QString tag = solution.fields()[std::size_t(field)].tag;
    for (const Node* p : solution.model().physics())
        if (p->tag == tag) a_node = p;
}

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

std::vector<const Node*> Problem::materials()
{
    const Topology& topo = a_s.topology();
    std::vector<const Node*> material(topo.domains.size(), nullptr);
    QString why;
    for (const Node& m : a_s.model().materials().children) {
        if (!m.enabled) continue;
        const QVector<int> on = resolveSelection(topo, a_s.model(), Level::Domain, m.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(m.name(), why));
        for (int d : on) material[std::size_t(d)] = &m;
    }
    return material;
}

QString Problem::materialProperty(const Node& m, const QString& key)
{
    QString text = m.text(key);
    // Not given: its library entry's ({"material": "FR-4"} in a file).
    if (text.trimmed().isEmpty() && !m.text(QStringLiteral("library")).isEmpty())
        if (const std::optional<MaterialEntry> entry = findMaterial(m.text(QStringLiteral("library"))))
            text = entry->properties.value(key).toString();
    return text;
}

std::unique_ptr<Problem> makeScalarProblem(Solution& solution, int field);
std::unique_ptr<Problem> makeElasticProblem(Solution& solution, int field);

std::unique_ptr<Problem> makeProblem(Solution& solution, int field)
{
    if (solution.fields()[std::size_t(field)].type == QLatin1String("solid")) return makeElasticProblem(solution, field);
    return makeScalarProblem(solution, field);
}

} // namespace detail

using detail::NaN;
using detail::tr;

// ------------------------------------------------------------------ Solution

namespace {

/// A physics as its variables need it.
struct FieldInfo {
    QString tag, type, variable;
    QStringList components;
    Dim dim;
    bool planeStress = false;   // (solid)
};

FieldInfo infoOf(const Node& p)
{
    FieldInfo f;
    f.tag = p.tag;
    f.type = p.type;
    if (p.type == QLatin1String("heat")) {
        f.variable = QStringLiteral("T");
        f.dim = Dim::temperature();
    } else if (p.type == QLatin1String("solid")) {
        f.variable = QStringLiteral("u");
        f.dim = Dim::length();
        f.planeStress = p.text(QStringLiteral("model")) == QLatin1String("planestress");
    } else {
        f.variable = QStringLiteral("V");
        f.dim = Dim::voltage();
    }
    f.components = p.type == QLatin1String("solid") ? QStringList{QStringLiteral("u"), QStringLiteral("v")} : QStringList{f.variable};
    return f;
}

bool axisymmetricModel(const Model& model)
{
    return model.component().text(QStringLiteral("space")) == QLatin1String("axisymmetric");
}

} // namespace

Solution::Solution(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Mesh> mesh)
    : a_model(model), a_params(std::move(parameters)), a_mesh(std::move(mesh))
{
    QString why;
    const std::optional<double> d = evaluateConstant(a_model.component().text(QStringLiteral("thickness")), a_params->scope,
                                                     Dim::length(), &why);
    a_thickness = d && *d > 0 ? *d : 1.0;
    a_axisymmetric = axisymmetricModel(a_model);
    std::map<int, std::shared_ptr<const Space>> spaces;
    for (const Node* p : a_model.physics()) {
        if (!p->enabled) continue;
        const FieldInfo info = infoOf(*p);
        Field f;
        f.tag = info.tag;
        f.type = info.type;
        f.variable = info.variable;
        f.components = info.components;
        f.dim = info.dim;
        f.initial = p->type == QLatin1String("heat") ? 293.15 : 0.0;
        const int order = p->text(QStringLiteral("order")) == QLatin1String("1") ? 1 : 2;
        auto it = spaces.find(order);
        if (it == spaces.end()) it = spaces.emplace(order, std::make_shared<Space>(a_mesh, order)).first;
        f.space = it->second;
        f.values.assign(std::size_t(f.space->size()) * std::size_t(f.count()), f.initial);
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
    // A parameter of a plain name (t, h, x, T...) keeps it: no such variable
    // (es.V, ht.T are the fields' still).
    if (!name.contains(QLatin1Char('.')) && a_params->scope.constant(name)) return -1;
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

using K = Solution::VariableDef::Kind;

/// How a variable is declared: its name, unit, what it is, its kind, its
/// field and component, a coefficient's name, a derived one's function.
using DefineFunction = std::function<int(const QString& name, const Dim& dim, const QString& description, K kind, int field,
                                         int component, const QString& coefficient, std::function<double(PointEvaluator&)> derived)>;

/// The stress state of a solid at a point: strains, stresses (SI).
struct StressState {
    double e1 = 0, e2 = 0, e3 = 0, g12 = 0;   // εx, εy, εz (εθ about the axis), γxy (engineering)
    double s1 = 0, s2 = 0, s3 = 0, s12 = 0;   // σx, σy, σz (σθ), τxy
    double elastic = 0;                        // strain energy density
};

StressState stressAt(PointEvaluator& e, int field, bool planeStress, bool axisymmetric, int sE, int sNu, int sAlpha, int sDT)
{
    StressState st;
    double u = 0, ux = 0, uy = 0, v = 0, vx = 0, vy = 0;
    e.component(field, 0, u, ux, uy);
    e.component(field, 1, v, vx, vy);
    const double E = e.value(sE), nu = e.value(sNu);
    const double a = e.value(sAlpha), dT = e.value(sDT);
    const double th = (std::isfinite(a) ? a : 0) * (std::isfinite(dT) ? dT : 0);   // the free thermal strain
    st.e1 = ux;
    st.e2 = vy;
    st.g12 = uy + vx;
    const double mu = E / (2 * (1 + nu));
    if (axisymmetric) {
        const double r = e.point().x();
        st.e3 = r > 0 ? u / r : ux;
        const double lambda = E * nu / ((1 + nu) * (1 - 2 * nu));
        const double tr = st.e1 + st.e2 + st.e3;
        const double t = (3 * lambda + 2 * mu) * th;
        st.s1 = lambda * tr + 2 * mu * st.e1 - t;
        st.s2 = lambda * tr + 2 * mu * st.e2 - t;
        st.s3 = lambda * tr + 2 * mu * st.e3 - t;
    } else if (planeStress) {
        const double f = E / (1 - nu * nu);
        st.s1 = f * ((st.e1 - th) + nu * (st.e2 - th));
        st.s2 = f * (nu * (st.e1 - th) + (st.e2 - th));
        st.s3 = 0;
        st.e3 = -nu / (1 - nu) * (st.e1 + st.e2 - 2 * th) + th;
    } else {
        const double lambda = E * nu / ((1 + nu) * (1 - 2 * nu));
        const double tr = st.e1 + st.e2;
        const double t = (3 * lambda + 2 * mu) * th;
        st.s1 = lambda * tr + 2 * mu * st.e1 - t;
        st.s2 = lambda * tr + 2 * mu * st.e2 - t;
        st.s3 = lambda * tr - t;
        st.e3 = 0;
    }
    st.s12 = mu * st.g12;
    st.elastic = 0.5 * (st.s1 * (st.e1 - th) + st.s2 * (st.e2 - th) + st.s3 * (st.e3 - th) + st.s12 * st.g12);
    return st;
}

/// The variables of a solution of \a fields, each to \a define: x, y (r, z
/// about the axis), t, the fields and what follows from them.
void declareVariables(const std::vector<FieldInfo>& fields, bool axisymmetric, const DefineFunction& defineVariable)
{
    auto define = [&](const QString& name, const Dim& dim, const QString& description, K kind, int field = -1,
                      std::function<double(PointEvaluator&)> derived = {}) {
        return defineVariable(name, dim, description, kind, field, 0, QString(), std::move(derived));
    };
    auto component = [&](const QString& name, const Dim& dim, const QString& description, K kind, int field, int c) {
        return defineVariable(name, dim, description, kind, field, c, QString(), {});
    };
    auto coefficient = [&](const QString& name, const Dim& dim, const QString& description, int field, const QString& which) {
        return defineVariable(name, dim, description, K::Coefficient, field, 0, which, {});
    };
    define(QStringLiteral("x"), Dim::length(), axisymmetric ? tr("x coordinate (r)") : tr("x coordinate"), K::X);
    define(QStringLiteral("y"), Dim::length(), axisymmetric ? tr("y coordinate (z)") : tr("y coordinate"), K::Y);
    if (axisymmetric) {
        define(QStringLiteral("r"), Dim::length(), tr("the distance from the axis"), K::X);
        define(QStringLiteral("z"), Dim::length(), tr("the position along the axis"), K::Y);
    }
    define(QStringLiteral("t"), Dim::of(0, 0, 1), tr("time"), K::Time);
    define(QStringLiteral("time"), Dim::of(0, 0, 1), tr("time (as t; where a parameter is named t)"), K::Time);
    define(QStringLiteral("dom"), Dim(), tr("the domain's number"), K::DomainNumber);
    define(QStringLiteral("h"), Dim::length(), tr("the mesh element's size"), K::Size);
    const Dim efield = Dim::of(1, 1, -3, -1), dfield = Dim::of(-2, 0, 1, 1), jfield = Dim::of(-2, 0, 0, 1);
    const Dim power = Dim::of(-1, 1, -3), gradT = Dim::of(-1, 0, 0, 0, 1), heatFlux = Dim::of(0, 1, -3);
    const Dim pressure = Dim::of(-1, 1, -2), energyDensity = Dim::of(-1, 1, -2);
    for (int f = 0; f < int(fields.size()); ++f) {
        const FieldInfo& field = fields[std::size_t(f)];
        const QString p = field.tag + QLatin1Char('.');
        if (field.type == QLatin1String("solid")) {
            const QString a = axisymmetric ? tr("radial") : tr("x");
            const QString b = axisymmetric ? tr("axial") : tr("y");
            component(p + QStringLiteral("u"), Dim::length(), tr("displacement, %1 component").arg(a), K::Value, f, 0);
            component(p + QStringLiteral("v"), Dim::length(), tr("displacement, %1 component").arg(b), K::Value, f, 1);
            component(QStringLiteral("u"), Dim::length(), tr("displacement, %1 component (%2)").arg(a, field.tag), K::Value, f, 0);
            component(QStringLiteral("v"), Dim::length(), tr("displacement, %1 component (%2)").arg(b, field.tag), K::Value, f, 1);
            const int su = component(p + QStringLiteral("u"), Dim::length(), QString(), K::Value, f, 0);
            const int sv = component(p + QStringLiteral("v"), Dim::length(), QString(), K::Value, f, 1);
            define(p + QStringLiteral("disp"), Dim::length(), tr("displacement, magnitude"), K::Derived, f,
                   [su, sv](PointEvaluator& e) { return std::hypot(e.value(su), e.value(sv)); });
            const int sE = coefficient(p + QStringLiteral("E"), pressure, tr("Young's modulus"), f, QStringLiteral("E"));
            const int sNu = coefficient(p + QStringLiteral("nu"), Dim(), tr("Poisson's ratio"), f, QStringLiteral("nu"));
            const int sA = coefficient(p + QStringLiteral("alpha"), Dim::of(0, 0, 0, 0, -1), tr("coefficient of thermal expansion"), f,
                                       QStringLiteral("alpha"));
            const int sT = coefficient(p + QStringLiteral("dT"), Dim::temperature(), tr("temperature above the strain-free one"), f,
                                       QStringLiteral("dT"));
            const bool ps = field.planeStress;
            auto st = [f, ps, axisymmetric, sE, sNu, sA, sT](PointEvaluator& e) { return stressAt(e, f, ps, axisymmetric, sE, sNu, sA, sT); };
            struct Out {
                const char* plane;
                const char* axi;
                Dim dim;
                QString what;
                double StressState::*member;
                double factor;
            };
            const Out outs[] = {
                {"ex", "er", Dim(), tr("strain, %1 component").arg(a), &StressState::e1, 1},
                {"ey", "ez", Dim(), tr("strain, %1 component").arg(b), &StressState::e2, 1},
                {"ez", "ephi", Dim(), axisymmetric ? tr("strain, hoop component") : tr("strain, out of the plane"), &StressState::e3, 1},
                {"exy", "erz", Dim(), tr("shear strain (tensor: half the engineering shear)"), &StressState::g12, 0.5},
                {"sx", "sr", pressure, tr("stress, %1 component").arg(a), &StressState::s1, 1},
                {"sy", "sz", pressure, tr("stress, %1 component").arg(b), &StressState::s2, 1},
                {"sz", "sphi", pressure, axisymmetric ? tr("stress, hoop component") : tr("stress, out of the plane"), &StressState::s3, 1},
                {"sxy", "srz", pressure, tr("shear stress"), &StressState::s12, 1},
                {"Ws", "Ws", energyDensity, tr("strain energy density"), &StressState::elastic, 1},
            };
            for (const Out& o : outs) {
                const QString name = QString::fromLatin1(axisymmetric ? o.axi : o.plane);
                const auto member = o.member;
                const double factor = o.factor;
                define(p + name, o.dim, o.what, K::Derived, f, [st, member, factor](PointEvaluator& e) { return factor * (st(e).*member); });
            }
            define(p + QStringLiteral("mises"), pressure, tr("von Mises stress"), K::Derived, f, [st](PointEvaluator& e) {
                const StressState s = st(e);
                return std::sqrt(0.5 * ((s.s1 - s.s2) * (s.s1 - s.s2) + (s.s2 - s.s3) * (s.s2 - s.s3) + (s.s3 - s.s1) * (s.s3 - s.s1))
                                 + 3 * s.s12 * s.s12);
            });
            define(p + QStringLiteral("p"), pressure, tr("pressure: minus the mean of the normal stresses"), K::Derived, f,
                   [st](PointEvaluator& e) {
                       const StressState s = st(e);
                       return -(s.s1 + s.s2 + s.s3) / 3;
                   });
            continue;
        }
        define(p + field.variable, field.dim, tr("the dependent variable"), K::Value, f, {});
        // Its plain name too (V, T), when no physics before has it.
        define(field.variable, field.dim, tr("the dependent variable of %1").arg(field.tag), K::Value, f, {});
        if (field.type == QLatin1String("electrostatics") || field.type == QLatin1String("currents")) {
            const int ex = define(p + QStringLiteral("Ex"), efield, tr("electric field, x component"), K::GradX, f);
            const int ey = define(p + QStringLiteral("Ey"), efield, tr("electric field, y component"), K::GradY, f);
            const int ne = define(p + QStringLiteral("normE"), efield, tr("electric field, norm"), K::GradNorm, f);
            if (field.type == QLatin1String("electrostatics")) {
                const int er = coefficient(p + QStringLiteral("epsr"), Dim(), tr("relative permittivity"), f, QString());
                const int dx = define(p + QStringLiteral("Dx"), dfield, tr("electric displacement, x component"), K::Derived, f,
                                      [er, ex](PointEvaluator& e) { return detail::Eps0 * e.value(er) * e.value(ex); });
                const int dy = define(p + QStringLiteral("Dy"), dfield, tr("electric displacement, y component"), K::Derived, f,
                                      [er, ey](PointEvaluator& e) { return detail::Eps0 * e.value(er) * e.value(ey); });
                define(p + QStringLiteral("normD"), dfield, tr("electric displacement, norm"), K::Derived, f,
                       [dx, dy](PointEvaluator& e) { return std::hypot(e.value(dx), e.value(dy)); });
                define(p + QStringLiteral("We"), Dim::of(-1, 1, -2), tr("electric energy density"), K::Derived, f,
                       [er, ne](PointEvaluator& e) {
                           const double n = e.value(ne);
                           return 0.5 * detail::Eps0 * e.value(er) * n * n;
                       });
            } else {
                const int sg = coefficient(p + QStringLiteral("sigma"), Dim::of(-3, -1, 3, 2), tr("electrical conductivity"), f, QString());
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
            const int k = coefficient(p + QStringLiteral("k"), Dim::of(1, 1, -3, 0, -1), tr("thermal conductivity"), f, QString());
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
    for (const Node* p : model.physics())
        if (p->enabled) out.push_back(infoOf(*p));
    return out;
}

} // namespace

QList<Variable> modelVariables(const Model& model)
{
    QList<Variable> out;
    QSet<QString> names;
    declareVariables(fieldInfos(model), axisymmetricModel(model),
                     [&](const QString& name, const Dim& dim, const QString& description, K, int, int, const QString&,
                         std::function<double(PointEvaluator&)>) {
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
    for (const Field& f : a_fields) {
        FieldInfo info;
        info.tag = f.tag;
        info.type = f.type;
        info.variable = f.variable;
        info.components = f.components;
        info.dim = f.dim;
        for (const Node* p : a_model.physics())
            if (p->tag == f.tag) info.planeStress = p->text(QStringLiteral("model")) == QLatin1String("planestress");
        fields.push_back(info);
    }
    declareVariables(fields, a_axisymmetric,
                     [this](const QString& name, const Dim& dim, const QString& description, VariableDef::Kind kind, int field,
                            int component, const QString& coefficient, std::function<double(PointEvaluator&)> derived) {
                         const bool known = a_pointScope->variable(name) >= 0;
                         const int slot = define(name, dim, description, kind, field, std::move(derived));
                         if (!known && slot >= 0) {
                             a_defs[std::size_t(slot)].component = component;
                             a_defs[std::size_t(slot)].coefficient = coefficient;
                         }
                         return slot;
                     });
    a_globalScope = std::make_unique<Scope>(a_pointScope.get());
}

bool Solution::varies(const Expression& e) const
{
    for (int slot : e.variables()) {
        const VariableDef& d = a_defs[std::size_t(slot)];
        if (d.kind != VariableDef::X && d.kind != VariableDef::Y && d.kind != VariableDef::DomainNumber && d.kind != VariableDef::Size)
            return true;
    }
    return false;
}

bool Solution::reads(const Expression& e, int f) const
{
    for (int slot : e.variables())
        if (a_defs[std::size_t(slot)].field == f) return true;
    return false;
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
            if (!description.isEmpty()) v.description = description;
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

void Solution::addSnapshot()
{
    Snapshot s;
    s.time = a_time;
    for (const Field& f : a_fields) {
        s.values.push_back(f.values);
        s.terminals.push_back(f.terminals);
    }
    for (const Variable& v : a_globals) s.globals.push_back(a_globalScope->constant(v.name)->value);
    a_snapshots.push_back(std::move(s));
    a_selected = int(a_snapshots.size()) - 1;
}

void Solution::select(int i)
{
    if (a_snapshots.empty()) return;
    if (i < 0 || i >= int(a_snapshots.size())) i = int(a_snapshots.size()) - 1;
    if (i == a_selected) return;
    const Snapshot& s = a_snapshots[std::size_t(i)];
    for (std::size_t f = 0; f < a_fields.size() && f < s.values.size(); ++f) {
        a_fields[f].values = s.values[f];
        a_fields[f].terminals = s.terminals[f];
    }
    a_time = s.time;
    for (int g = 0; g < int(a_globals.size()) && g < int(s.globals.size()); ++g)
        a_globalScope->setConstant(a_globals[g].name, s.globals[std::size_t(g)], a_globals[g].dim);
    a_selected = i;
}

int Solution::snapshotAt(double t) const
{
    int best = -1;
    double gap = INFINITY;
    for (int i = 0; i < int(a_snapshots.size()); ++i) {
        const double d = std::abs(a_snapshots[std::size_t(i)].time - t);
        if (d < gap) {
            gap = d;
            best = i;
        }
    }
    return best;
}

// ------------------------------------------------------------------ PointEvaluator

PointEvaluator::PointEvaluator(const Solution& solution) : a_s(solution)
{
    a_slots.assign(std::size_t(solution.pointScope().slotCount()), NaN);
    a_done.assign(a_slots.size(), 0);
    a_fields.resize(solution.fields().size());
    a_offset.assign(solution.fields().size(), 0);
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

void PointEvaluator::perturb(int f, double du)
{
    if (f < 0 || f >= int(a_offset.size())) return;
    a_offset[std::size_t(f)] = du;
    std::fill(a_done.begin(), a_done.end(), 0);
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

void PointEvaluator::component(int f, int comp, double& u, double& gx, double& gy)
{
    FieldCache& c = a_fields[std::size_t(f)];
    const Field& field = a_s.fields()[std::size_t(f)];
    if (!c.done) {
        const Space& sp = *field.space;
        const int d = a_s.mesh().domain[std::size_t(a_t)];
        if (field.solved && (d < 0 || !field.domains[std::size_t(d)])) {
            for (int k = 0; k < 2; ++k) c.u[k] = c.gx[k] = c.gy[k] = NaN;
        } else {
            Shape s;
            shapeAt(sp.order(), a_xi, a_eta, s);
            const Space::Map m = sp.map(a_t, a_xi, a_eta);
            const int* dofs = sp.dofs(a_t);
            const std::size_t n = std::size_t(sp.size());
            for (int k = 0; k < std::min(2, field.count()); ++k) {
                double uu = 0, rx = 0, ry = 0;
                for (int i = 0; i < s.count; ++i) {
                    const double v = field.values[std::size_t(k) * n + std::size_t(dofs[i])];
                    uu += s.n[i] * v;
                    rx += s.dxi[i] * v;
                    ry += s.deta[i] * v;
                }
                c.u[k] = uu;
                Space::gradient(m, rx, ry, c.gx[k], c.gy[k]);
            }
        }
        c.done = true;
    }
    if (comp < 0 || comp > 1) {
        u = gx = gy = NaN;
        return;
    }
    u = c.u[comp] + (comp == 0 ? a_offset[std::size_t(f)] : 0.0);
    gx = c.gx[comp];
    gy = c.gy[comp];
}

double PointEvaluator::coefficient(int f, const QString& name)
{
    const int dom = a_s.mesh().domain[std::size_t(a_t)];
    const Solution::Coefficients& coefs = a_s.coefficients()[std::size_t(f)];
    const std::vector<Expression>* list = &coefs.c;
    if (!name.isEmpty()) {
        auto it = coefs.named.find(name);
        if (it == coefs.named.end()) return NaN;
        list = &it->second;
    }
    if (dom < 0 || dom >= int(list->size()) || !(*list)[std::size_t(dom)].isValid() || a_depth >= 8) return NaN;
    ++a_depth;
    const double v = eval((*list)[std::size_t(dom)]);
    --a_depth;
    return v;
}

double PointEvaluator::value(int slot)
{
    if (slot < 0 || slot >= int(a_slots.size())) return NaN;
    if (a_done[std::size_t(slot)]) return a_slots[std::size_t(slot)];
    const Solution::VariableDef& d = a_s.definitions()[std::size_t(slot)];
    double v = NaN;
    switch (d.kind) {
    case K::X: v = point().x(); break;
    case K::Y: v = point().y(); break;
    case K::Time: v = a_s.time(); break;
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
        component(d.field, d.component, u, gx, gy);
        // The electric field is minus the potential's gradient.
        const QString& type = a_s.fields()[std::size_t(d.field)].type;
        const double sign = type == QLatin1String("electrostatics") || type == QLatin1String("currents") ? -1 : 1;
        v = d.kind == K::Value ? u : d.kind == K::GradX ? sign * gx : d.kind == K::GradY ? sign * gy : std::hypot(gx, gy);
        break;
    }
    case K::Coefficient: v = coefficient(d.field, d.coefficient); break;
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

// ------------------------------------------------------------------ Value lists

namespace {

/// \a text split at its commas outside brackets.
QStringList topLevelItems(const QString& text)
{
    QStringList out;
    int depth = 0;
    QString current;
    for (QChar c : text) {
        if (c == QLatin1Char('(') || c == QLatin1Char('[')) ++depth;
        else if (c == QLatin1Char(')') || c == QLatin1Char(']')) --depth;
        if ((c == QLatin1Char(',') || c == QLatin1Char(';')) && depth == 0) {
            out << current.trimmed();
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.trimmed().isEmpty() || !out.isEmpty()) out << current.trimmed();
    out.removeAll(QString());
    return out;
}

} // namespace

std::optional<std::vector<double>> valueList(const QString& text, const Scope& scope, Dim* dimOut, QString* error)
{
    std::vector<double> out;
    std::optional<Dim> dim;
    auto fail = [&](const QString& why) -> std::optional<std::vector<double>> {
        if (error) *error = why;
        return std::nullopt;
    };
    auto value = [&](const QString& t, double* v, Dim* d) {
        const Expression e = Expression::compile(t, scope);
        if (!e.isValid()) {
            if (error) *error = tr("%1: %2").arg(t, e.error());
            return false;
        }
        if (!e.isConstant()) {
            if (error) *error = tr("%1 is not a constant").arg(t);
            return false;
        }
        *v = e.constant();
        *d = e.dim();
        return true;
    };
    // (A plain 0 is 0 in whatever unit the others have.)
    auto agree = [&](const Dim& d, const QString& t, double v) {
        if (d.isNone() && v == 0) return true;
        if (!dim) {
            dim = d;
            return true;
        }
        if (!(*dim == d)) {
            if (error) *error = tr("%1 is in %2, where the others are in %3").arg(t, dimName(d), dimName(*dim));
            return false;
        }
        return true;
    };
    static const QRegularExpression call(QStringLiteral("^\\s*(range|linspace)\\s*\\((.*)\\)\\s*$"));
    for (const QString& item : topLevelItems(text)) {
        const QRegularExpressionMatch m = call.match(item);
        if (m.hasMatch()) {
            const QStringList args = topLevelItems(m.captured(2));
            if (args.size() != 3) return fail(tr("%1(...) takes three values: %2").arg(m.captured(1), item));
            double a = 0, b = 0, c = 0;
            Dim da, db, dc;
            if (!value(args[0], &a, &da) || !value(args[1], &b, &db) || !value(args[2], &c, &dc)) return std::nullopt;
            if (m.captured(1) == QLatin1String("range")) {
                // range(start, step, stop)
                if (!agree(da, args[0], a) || !agree(db, args[1], b) || !agree(dc, args[2], c)) return std::nullopt;
                if (b == 0 || (c - a) / b < 0) return fail(tr("%1: its step does not lead from its start to its stop").arg(item));
                const double n = std::floor((c - a) / b + 1e-9);
                if (n > 1e6) return fail(tr("%1: more than a million values").arg(item));
                for (int i = 0; i <= int(n); ++i) out.push_back(a + i * b);
            } else {
                // linspace(start, stop, count)
                if (!agree(da, args[0], a) || !agree(db, args[1], b)) return std::nullopt;
                const int n = int(std::lround(c));
                if (n < 1 || n > 1000000) return fail(tr("%1: from 1 to a million values").arg(item));
                for (int i = 0; i < n; ++i) out.push_back(n == 1 ? a : a + (b - a) * i / (n - 1));
            }
            continue;
        }
        double v = 0;
        Dim d;
        if (!value(item, &v, &d)) return std::nullopt;
        if (!agree(d, item, v)) return std::nullopt;
        out.push_back(v);
    }
    if (out.empty()) return fail(tr("no values"));
    if (dimOut) *dimOut = dim.value_or(Dim());
    return out;
}

// ------------------------------------------------------------------ Studies

namespace {

using detail::Problem;
using detail::SolveOptions;

struct Step {
    Solution& s;
    const Node& node;
    std::vector<std::unique_ptr<Problem>> problems;
    std::vector<int> which;   // the fields, by problem
    SolveOptions options;
    int maxIterations = 50;
    double tolerance = 1e-6;
};

/// The problems of \a step: its physics (all of them when it names none).
bool setupStep(Step& st, QString* error)
{
    QStringList tags = st.node.list(QStringLiteral("physics"));
    if (tags.isEmpty())
        for (const Field& f : st.s.fields()) tags << f.tag;
    QStringList errors;
    for (const QString& t : tags) {
        const int fi = st.s.fieldIndex(t);
        if (fi < 0) {
            *error = tr("%1: there is no physics %2 (enabled) to solve").arg(st.node.name(), t);
            return false;
        }
        st.which.push_back(fi);
        auto p = detail::makeProblem(st.s, fi);
        QString why;
        if (!p->setup(&why)) errors << why;
        st.problems.push_back(std::move(p));
    }
    if (!errors.isEmpty()) {
        *error = errors.join(QLatin1Char('\n'));
        return false;
    }
    st.options.solver = st.node.text(QStringLiteral("solver"));
    st.options.nonlinear = st.node.text(QStringLiteral("nonlinear"));
    if (st.options.nonlinear.isEmpty()) st.options.nonlinear = QStringLiteral("auto");
    if (const std::optional<double> t = evaluateConstant(st.node.text(QStringLiteral("tolerance")), st.s.parameters().scope, Dim()); t && *t > 0)
        st.tolerance = *t;
    st.options.tolerance = st.tolerance;
    st.maxIterations = std::clamp(st.node.integer(QStringLiteral("maxiter")), 1, 1000);
    st.options.maxIterations = st.maxIterations;
    return true;
}

/// Whether the step's physics must be solved in turn till they agree:
/// one reads another's field (or its own, when not by Newton).
bool coupled(const Step& st, const std::vector<int>& subset)
{
    for (int i : subset)
        for (int j : subset) {
            if (i == j && st.options.nonlinear != QLatin1String("picard")) continue;
            if (st.problems[std::size_t(i)]->reads(st.which[std::size_t(j)])) return true;
        }
    return false;
}

/// Solves the problems \a subset of \a st, in turn till they agree. \a
/// optionsOf gives each one's options; \a iterationsUsed how many rounds.
bool solveTogether(Step& st, const std::vector<int>& subset, const std::function<SolveOptions(int)>& optionsOf, bool log, QString* error,
                   int* iterationsUsed = nullptr)
{
    const bool loop = coupled(st, subset);
    const int iterations = loop ? st.maxIterations : 1;
    for (int it = 0; it < iterations; ++it) {
        double change = 0;
        for (int i : subset) {
            Field& f = st.s.fields()[std::size_t(st.which[std::size_t(i)])];
            const std::vector<double> before = f.values;
            if (!st.problems[std::size_t(i)]->solve(optionsOf(i), error)) return false;
            double num = 0, den = 0;
            for (std::size_t k = 0; k < f.values.size() && k < before.size(); ++k) {
                const double d = f.values[k] - before[k];
                num += d * d;
                den += f.values[k] * f.values[k];
            }
            change = std::max(change, den > 0 ? std::sqrt(num / den) : std::sqrt(num));
        }
        if (iterationsUsed) *iterationsUsed = it + 1;
        if (!loop) return true;
        if (log) st.s.log << tr("%1, iteration %2: relative change %3").arg(st.node.name()).arg(it + 1).arg(change, 0, 'g', 3);
        if (change < st.tolerance && it > 0) return true;
    }
    *error = tr("%1: the coupled physics did not converge in %2 iterations").arg(st.node.name()).arg(st.maxIterations);
    return false;
}

bool stationaryStep(Solution& s, const Node& node, const std::function<bool(double, const QString&)>& tell, QStringList* warnings, QString* error)
{
    Step st{s, node, {}, {}, {}, 50, 1e-6};
    if (!setupStep(st, error)) return false;
    // A field not solved before starts from its initial values (a step
    // before leaves its own as the next one's start).
    for (auto& p : st.problems)
        if (!p->field().solved) p->initialize();
    const bool sweep = node.flag(QStringLiteral("sweep"));
    std::vector<int> all;
    for (int i = 0; i < int(st.problems.size()); ++i) all.push_back(i);
    if (!tell(0.1, tr("Solving %1").arg(node.name()))) {
        *error = tr("cancelled");
        return false;
    }
    QString why;
    if (!solveTogether(st, all, [&](int) { return st.options; }, true, &why)) {
        if (why.contains(QLatin1String("did not converge"))) *warnings << why;
        else {
            *error = why;
            return false;
        }
    }
    if (sweep) {
        // Once more with the terminals swept.
        SolveOptions o = st.options;
        o.sweep = true;
        for (auto& p : st.problems)
            if (p->field().type == QLatin1String("electrostatics") || p->field().type == QLatin1String("currents"))
                if (!p->solve(o, error)) return false;
    }
    for (auto& p : st.problems) p->setGlobals();
    return true;
}

bool transientStep(Solution& s, const Node& node, const std::function<bool(double, const QString&)>& tell, QStringList* warnings, QString* error)
{
    Step st{s, node, {}, {}, {}, 50, 1e-6};
    if (!setupStep(st, error)) return false;
    Dim dim;
    QString why;
    std::optional<std::vector<double>> listed = valueList(node.text(QStringLiteral("times")), s.parameters().scope, &dim, &why);
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
    for (int i = 0; i < int(st.problems.size()); ++i) (st.problems[std::size_t(i)]->hasMass() ? stepped : quasi).push_back(i);
    if (stepped.empty())
        *warnings << tr("%1: none of its physics changes in time (only heat does): each output time is solved at rest").arg(node.name());
    double rtol = 1e-3;
    if (const std::optional<double> r = evaluateConstant(node.text(QStringLiteral("rtol")), s.parameters().scope, Dim()); r && *r > 0) rtol = *r;
    const bool euler = node.text(QStringLiteral("method")) == QLatin1String("euler");
    const bool fixed = node.text(QStringLiteral("stepping")) == QLatin1String("fixed");
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
    // The start: initial values; what is at rest solved with them.
    s.setTime(t0);
    for (auto& p : st.problems) p->initialize();
    if (!quasi.empty() && !solveTogether(st, quasi, [&](int) { return st.options; }, false, error)) return false;
    for (auto& p : st.problems) p->setGlobals();
    s.addSnapshot();
    s.timeDependent = true;
    // History of the stepped fields: (time, values) newest first.
    struct History {
        std::vector<double> t;
        std::vector<std::vector<double>> u;
    };
    std::vector<History> hist(st.problems.size());
    for (int i : stepped) {
        hist[std::size_t(i)].t.push_back(t0);
        hist[std::size_t(i)].u.push_back(st.problems[std::size_t(i)]->field().values);
    }
    std::vector<std::vector<double>> u0(st.problems.size());
    for (int i : stepped) u0[std::size_t(i)] = st.problems[std::size_t(i)]->field().values;
    double t = t0, hPrev = 0;
    int accepted = 0, rejected = 0;
    std::size_t out = 1;
    std::vector<std::vector<double>> histories(st.problems.size());
    std::vector<double> masses(st.problems.size(), 0);
    const int maxSteps = 1000000;
    while (out < times.size()) {
        if (accepted + rejected > maxSteps) {
            *error = tr("%1: more than a million steps; stopped at t = %2").arg(node.name(), formatQuantity(t, Dim::of(0, 0, 1)));
            return false;
        }
        const double target = times[out];
        if (fixed) h = fixedStep;
        // To the output time exactly, no sliver left before it: what is left
        // of it taken in one step, or in two halves (never a longer step than
        // asked, so a step made smaller stays so).
        if (t + h >= target - 1e-9 * range) h = target - t;
        else if (target - (t + h) < 0.2 * h) h = (target - t) / 2;
        const int order = euler || accepted < 2 ? 1 : 2;
        const double omega = hPrev > 0 ? h / hPrev : 1;
        // Saved, to step again from here.
        std::vector<std::vector<double>> saved;
        for (const Field& f : s.fields()) saved.push_back(f.values);
        s.setTime(t + h);
        for (int i : stepped) {
            const History& hi = hist[std::size_t(i)];
            const std::vector<double>& un = hi.u[0];
            std::vector<double>& hv = histories[std::size_t(i)];
            hv.resize(un.size());
            if (order == 1) {
                masses[std::size_t(i)] = 1 / h;
                for (std::size_t k = 0; k < un.size(); ++k) hv[k] = un[k] / h;
            } else {
                const std::vector<double>& um = hi.u[1];
                masses[std::size_t(i)] = (1 + 2 * omega) / ((1 + omega) * h);
                const double a1 = (1 + omega), a2 = omega * omega / (1 + omega);
                for (std::size_t k = 0; k < un.size(); ++k) hv[k] = (a1 * un[k] - a2 * um[k]) / h;
            }
        }
        QString failed;
        const bool ok = solveTogether(st, [&] {
            std::vector<int> all;
            for (int i = 0; i < int(st.problems.size()); ++i) all.push_back(i);
            return all;
        }(), [&](int i) {
            SolveOptions o = st.options;
            if (st.problems[std::size_t(i)]->hasMass()) {
                o.mass = masses[std::size_t(i)];
                o.history = &histories[std::size_t(i)];
            }
            return o;
        }, false, &failed);
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
        // The error of the step: the solution against a predictor of its
        // order (Milne's device).
        double err = 0;
        if (!fixed && accepted >= 1) {
            for (int i : stepped) {
                const History& hi = hist[std::size_t(i)];
                const std::vector<double>& uc = st.problems[std::size_t(i)]->field().values;
                const double h1 = hi.t[0] - hi.t[1];
                double ratio = 0;
                std::vector<double> pred(uc.size());
                if (order == 1 || hi.u.size() < 3) {
                    // Linear through the last two; BE's error h/(2h + h1) of the gap.
                    const double w = h / h1;
                    for (std::size_t k = 0; k < uc.size(); ++k) pred[k] = hi.u[0][k] + w * (hi.u[0][k] - hi.u[1][k]);
                    ratio = order == 1 ? h / (2 * h + h1) : 1.0 / 3;
                } else {
                    // Quadratic through the last three (Lagrange at t + h).
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
            hi.u.insert(hi.u.begin(), st.problems[std::size_t(i)]->field().values);
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
            for (auto& p : st.problems) p->setGlobals();
            s.addSnapshot();
            ++out;
        }
        if (!fixed) h *= err > 0 ? std::clamp(0.9 * std::pow(err, -1.0 / (order + 1)), 0.2, 2.0) : 2.0;
        if (h < hmin) {
            *error = tr("%1: the time step became too small at t = %2").arg(node.name(), formatQuantity(t, Dim::of(0, 0, 1)));
            return false;
        }
        if (!tell(0.05 + 0.9 * (t - t0) / range, tr("%1: t = %2").arg(node.name(), formatQuantity(t, Dim::of(0, 0, 1))))) {
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
    if (solution->axisymmetric()) {
        const QRectF b = mesh->bounds();
        if (b.left() < -1e-9 * std::max(b.width(), b.height())) {
            out.error = tr("an axisymmetric model lies where r ≥ 0: its geometry reaches x = %1").arg(formatNumber(b.left(), 6));
            return out;
        }
    }
    std::vector<const Node*> steps;
    for (const Node& step : study->children)
        if (step.enabled && step.type != QLatin1String("sweep")) steps.push_back(&step);
    if (steps.empty()) {
        out.error = tr("%1 has no step to solve").arg(study->name());
        return out;
    }
    for (std::size_t k = 0; k < steps.size(); ++k) {
        const Node& step = *steps[k];
        auto tell = [&](double f, const QString& what) {
            return !progress || progress((double(k) + std::clamp(f, 0.0, 1.0)) / double(steps.size()), what);
        };
        QString why;
        bool ok = false;
        if (step.type == QLatin1String("stationary")) ok = stationaryStep(*solution, step, tell, &out.warnings, &why);
        else if (step.type == QLatin1String("transient")) ok = transientStep(*solution, step, tell, &out.warnings, &why);
        else why = tr("%1: a %2 step is not solved yet").arg(study->name(), step.name());
        if (!ok) {
            out.error = why;
            return out;
        }
    }
    solution->seconds = timer.elapsed() / 1000.0;
    int dofs = 0;
    for (const Field& f : solution->fields())
        if (f.solved) dofs += f.space->size() * f.count();
    solution->log << tr("Solved %1 degrees of freedom in %2 s").arg(dofs).arg(solution->seconds, 0, 'f', 2);
    if (progress) progress(1.0, tr("Done"));
    out.solution = solution;
    return out;
}

namespace {

/// Whether a parameter named one of \a names is in \a node (an identifier in
/// any of its texts).
bool mentions(const QJsonValue& value, const QSet<QString>& names)
{
    if (names.isEmpty()) return false;
    const QString text = value.isObject() ? QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact))
                         : value.isArray() ? QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact))
                                           : value.toVariant().toString();
    static const QRegularExpression identifier(QStringLiteral("[A-Za-z_][A-Za-z0-9_]*"));
    auto it = identifier.globalMatch(text);
    while (it.hasNext())
        if (names.contains(it.next().captured(0))) return true;
    return false;
}

/// A value as a parameter's expression: in SI, its unit after it.
QString valueText(double v, const Dim& dim)
{
    const QString number = QString::number(v, 'g', 17);
    return dim.isNone() ? number : number + QLatin1Char('[') + dimName(dim) + QLatin1Char(']');
}

} // namespace

StudyRun runStudy(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Topology> topology,
                  std::shared_ptr<const Mesh> mesh, const QString& studyTag, const Progress& progress)
{
    StudyRun run;
    run.mesh = mesh;
    const Node* study = nullptr;
    for (const Node* s : model.studies())
        if (studyTag.isEmpty() ? !study : s->tag == studyTag) study = s;
    if (!study) {
        run.error = studyTag.isEmpty() ? tr("the model has no study") : tr("there is no study %1").arg(studyTag);
        return run;
    }
    const Node* sweep = nullptr;
    for (const Node& c : study->children)
        if (c.type == QLatin1String("sweep") && c.enabled) sweep = &c;
    auto meshOf = [&](const ParameterScope& ps, std::shared_ptr<const Topology> topo, const Progress& p,
                      std::shared_ptr<const Mesh>* out) {
        if (!topo) {
            const GeometryBuild g = buildGeometry(model, ps);
            if (!g.ok()) {
                const Node* n = model.find(g.failedAt);
                run.error = tr("the geometry: %1: %2").arg(n ? n->name() : g.failedAt, g.errors.value(g.failedAt));
                return false;
            }
            topo = g.topology;
        }
        const MeshBuild m = buildMesh(model, ps, topo, p);
        if (!m.mesh) {
            run.error = tr("the mesh: %1").arg(m.error);
            return false;
        }
        run.warnings << m.warnings;
        *out = m.mesh;
        return true;
    };
    if (!sweep) {
        if (!run.mesh && !meshOf(*parameters, topology, [&](double f, const QString& w) { return !progress || progress(0.3 * f, w); }, &run.mesh))
            return run;
        const double from = mesh ? 0 : 0.3;
        const StudyResult r = solveStudy(model, parameters, run.mesh, study->tag,
                                         [&](double f, const QString& w) { return !progress || progress(from + (1 - from) * f, w); });
        run.warnings << r.warnings;
        if (!r.solution) {
            run.error = r.error;
            return run;
        }
        run.log = r.solution->log;
        run.solutions.push_back(r.solution);
        return run;
    }
    // The sweep's points.
    struct Swept {
        QString name;
        std::vector<double> values;
        Dim dim;
    };
    std::vector<Swept> lists;
    QStringList known;
    for (const Model::Parameter& p : model.parameters()) known << p.name.trimmed();
    for (const QJsonValue& row : sweep->value(QStringLiteral("parameters")).toArray()) {
        const QJsonArray r = row.toArray();
        const QString name = r.at(0).toString().trimmed();
        if (name.isEmpty()) continue;
        if (!known.contains(name)) {
            run.error = tr("%1: there is no parameter %2").arg(sweep->name(), name);
            return run;
        }
        QString why;
        Dim dim;
        std::optional<std::vector<double>> values = valueList(r.at(1).toString(), parameters->scope, &dim, &why);
        if (!values) {
            run.error = tr("%1: %2's values: %3").arg(sweep->name(), name, why);
            return run;
        }
        // Plain numbers in the unit given, else in the parameter's own.
        const QString unitText = r.at(2).toString().trimmed();
        const Dim own = parameters->dims.value(name);
        if (dim.isNone() && !unitText.isEmpty()) {
            const std::optional<Unit> u = parseUnit(unitText, &why);
            if (!u) {
                run.error = tr("%1: %2's unit: %3").arg(sweep->name(), name, why);
                return run;
            }
            for (double& v : *values) v = v * u->scale + u->offset;
            dim = u->dim;
        } else if (dim.isNone() && !own.isNone()) {
            run.error = tr("%1: %2 is in %3: give its values a unit (2[mm]), or the row one").arg(sweep->name(), name, dimName(own));
            return run;
        }
        lists.push_back({name, *values, dim});
        run.swept << name;
    }
    if (lists.empty()) {
        run.error = tr("%1 sweeps no parameter: name one and its values").arg(sweep->name());
        return run;
    }
    std::vector<std::vector<double>> points;   // each a value of each list
    if (sweep->text(QStringLiteral("combination")) == QLatin1String("specified")) {
        const std::size_t n = lists.front().values.size();
        for (const Swept& l : lists)
            if (l.values.size() != n) {
                run.error = tr("%1: in specified combinations every parameter has as many values (%2 has %3, %4 has %5)")
                                .arg(sweep->name(), lists.front().name)
                                .arg(n)
                                .arg(l.name)
                                .arg(l.values.size());
                return run;
            }
        for (std::size_t k = 0; k < n; ++k) {
            std::vector<double> p;
            for (const Swept& l : lists) p.push_back(l.values[k]);
            points.push_back(p);
        }
    } else {
        // Every combination; the last parameter changes fastest.
        std::size_t total = 1;
        for (const Swept& l : lists) total *= l.values.size();
        if (total > 10000) {
            run.error = tr("%1: %2 combinations; 10000 at most").arg(sweep->name()).arg(total);
            return run;
        }
        for (std::size_t k = 0; k < total; ++k) {
            std::vector<double> p(lists.size());
            std::size_t rest = k;
            for (int i = int(lists.size()) - 1; i >= 0; --i) {
                const std::size_t n = lists[std::size_t(i)].values.size();
                p[std::size_t(i)] = lists[std::size_t(i)].values[rest % n];
                rest /= n;
            }
            points.push_back(p);
        }
    }
    // What shapes the geometry and the mesh.
    const QJsonObject whole = model.toObject();
    const QJsonObject component = whole.value(QStringLiteral("components")).toArray().first().toObject();
    QJsonObject shaping;
    shaping.insert(QStringLiteral("geometry"), component.value(QStringLiteral("geometry")));
    shaping.insert(QStringLiteral("mesh"), component.value(QStringLiteral("mesh")));
    shaping.insert(QStringLiteral("functions"), whole.value(QStringLiteral("functions")));
    std::shared_ptr<const Mesh> base = mesh;
    for (std::size_t k = 0; k < points.size(); ++k) {
        QHash<QString, QString> overrides;
        QStringList label;
        QHash<QString, double> values;
        for (std::size_t i = 0; i < lists.size(); ++i) {
            overrides.insert(lists[i].name, valueText(points[k][i], lists[i].dim));
            label << QStringLiteral("%1 = %2").arg(lists[i].name, formatQuantity(points[k][i], lists[i].dim));
            values.insert(lists[i].name, points[k][i]);
        }
        const std::shared_ptr<ParameterScope> ps = evaluateParameters(model, overrides);
        if (!ps->errors.isEmpty()) {
            QStringList list;
            for (auto it = ps->errors.cbegin(); it != ps->errors.cend(); ++it) list << tr("%1: %2").arg(it.key(), it.value());
            run.error = tr("%1: the parameters: %2").arg(label.join(QLatin1String(", ")), list.join(QLatin1String("; ")));
            return run;
        }
        // The parameters it changes, and whether they shape the geometry.
        QSet<QString> changed;
        for (auto it = ps->values.cbegin(); it != ps->values.cend(); ++it)
            if (!parameters->values.contains(it.key()) || parameters->values.value(it.key()) != it.value()) changed.insert(it.key());
        const double from = double(k) / double(points.size()), span = 1.0 / double(points.size());
        const QString where = label.join(QLatin1String(", "));
        auto sub = [&](double a, double b) {
            return Progress([&, a, b](double f, const QString& w) {
                return !progress || progress(from + span * (a + (b - a) * f), QStringLiteral("%1: %2").arg(where, w));
            });
        };
        std::shared_ptr<const Mesh> m;
        if (mentions(shaping, changed)) {
            if (!meshOf(*ps, nullptr, sub(0, 0.3), &m)) {
                run.error = tr("%1: %2").arg(where, run.error);
                return run;
            }
        } else {
            if (!base && !meshOf(*parameters, topology, sub(0, 0.3), &base)) return run;
            run.mesh = base;
            m = base;
        }
        const StudyResult r = solveStudy(model, ps, m, study->tag, sub(0.3, 1));
        run.warnings << r.warnings;
        if (!r.solution) {
            run.error = tr("%1: %2").arg(where, r.error);
            return run;
        }
        r.solution->label = where;
        r.solution->sweptValues = values;
        run.log << QStringLiteral("%1: %2").arg(where, r.solution->log.join(QLatin1String("; ")));
        run.solutions.push_back(r.solution);
    }
    if (!run.mesh) run.mesh = base;
    return run;
}

double vacuumCapacitance(const Solution& solution, const QString& tag, const QString& terminal, QString* error)
{
    Solution& s = const_cast<Solution&>(solution);
    const int fi = s.fieldIndex(tag);
    if (fi < 0) {
        if (error) *error = tr("there is no physics %1").arg(tag);
        return NaN;
    }
    return detail::scalarVacuumCapacitance(s, fi, terminal, error);
}

} // namespace qucs_s::fem
