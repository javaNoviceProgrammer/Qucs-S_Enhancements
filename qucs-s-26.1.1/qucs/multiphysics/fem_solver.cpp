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

#include "fem_edge.h"
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

void buildPattern(int neq, const ElementPattern& elements, std::vector<int>& outer, std::vector<int>& inner)
{
    std::vector<std::vector<int>> rows(static_cast<std::size_t>(neq));
    elements([&](const int* eqs, int n) {
        for (int i = 0; i < n; ++i) {
            if (eqs[i] < 0) continue;
            std::vector<int>& row = rows[std::size_t(eqs[i])];
            for (int j = 0; j < n; ++j)
                if (eqs[j] >= 0) row.push_back(eqs[j]);
        }
    });
    outer.assign(std::size_t(neq) + 1, 0);
    for (int r = 0; r < neq; ++r) {
        std::vector<int>& row = rows[std::size_t(r)];
        std::sort(row.begin(), row.end());
        row.erase(std::unique(row.begin(), row.end()), row.end());
        if (!std::binary_search(row.begin(), row.end(), r)) row.insert(std::lower_bound(row.begin(), row.end(), r), r);
        outer[std::size_t(r) + 1] = outer[std::size_t(r)] + int(row.size());
    }
    inner.resize(std::size_t(outer.back()));
    for (int r = 0; r < neq; ++r) {
        std::copy(rows[std::size_t(r)].begin(), rows[std::size_t(r)].end(), inner.begin() + outer[std::size_t(r)]);
        std::vector<int>().swap(rows[std::size_t(r)]);
    }
}

void LinearSystem::setPattern(int neq, const ElementPattern& elements)
{
    a_n = neq;
    std::vector<int> outer, inner;
    buildPattern(neq, elements, outer, inner);
    // (Its pattern is symmetric: column j holds row j's columns.)
    a_A = Eigen::SparseMatrix<double>(neq, neq);
    a_A.resizeNonZeros(outer.back());
    std::copy(outer.begin(), outer.end(), a_A.outerIndexPtr());
    std::copy(inner.begin(), inner.end(), a_A.innerIndexPtr());
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

// ------------------------------------------------------------------ ComplexSystem

void ComplexSystem::setPattern(int neq, const ElementPattern& elements)
{
    a_n = neq;
    std::vector<int> outer, inner;
    buildPattern(neq, elements, outer, inner);
    a_A = Matrix(neq, neq);
    a_A.resizeNonZeros(outer.back());
    std::copy(outer.begin(), outer.end(), a_A.outerIndexPtr());
    std::copy(inner.begin(), inner.end(), a_A.innerIndexPtr());
    clear();
    a_lu.reset();
    a_analyzed = false;
    // Its real equivalent's pattern: each entry a 2 × 2 block.
    std::vector<Eigen::Triplet<double>> t;
    t.reserve(std::size_t(outer.back()) * 4);
    for (int j = 0; j < neq; ++j)
        for (int k = outer[std::size_t(j)]; k < outer[std::size_t(j) + 1]; ++k) {
            const int i = inner[std::size_t(k)];
            for (int a = 0; a < 2; ++a)
                for (int b = 0; b < 2; ++b) t.emplace_back(2 * i + a, 2 * j + b, 1.0);
        }
    a_real = Eigen::SparseMatrix<double>(2 * neq, 2 * neq);
    a_real.setFromTriplets(t.begin(), t.end());
    a_ldlt.reset();
    a_ldltAnalyzed = false;
}

void ComplexSystem::clear()
{
    std::fill(a_A.valuePtr(), a_A.valuePtr() + a_A.nonZeros(), std::complex<double>(0, 0));
}

void ComplexSystem::add(int i, int j, std::complex<double> v)
{
    const int* inner = a_A.innerIndexPtr();
    const int* outer = a_A.outerIndexPtr();
    const int* begin = inner + outer[j];
    const int* end = inner + outer[j + 1];
    const int* at = std::lower_bound(begin, end, i);
    a_A.valuePtr()[at - inner] += v;
}

bool ComplexSystem::factorize(const QString& name, QString* error, bool quasiDefinite)
{
    if (quasiDefinite) {
        // [Re, -Im; -Im, -Re] in each 2 × 2 block: its rows the real part's
        // equation and minus the imaginary part's.
        for (int j = 0; j < a_n; ++j)
            for (Matrix::InnerIterator it(a_A, j); it; ++it) {
                const int i = int(it.row());
                const double re = it.value().real(), im = it.value().imag();
                a_real.coeffRef(2 * i, 2 * j) = re;
                a_real.coeffRef(2 * i, 2 * j + 1) = -im;
                a_real.coeffRef(2 * i + 1, 2 * j) = -im;
                a_real.coeffRef(2 * i + 1, 2 * j + 1) = -re;
            }
        if (!a_ldlt) {
            a_ldlt = std::make_unique<Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>>>();
            a_ldltAnalyzed = false;
        }
        if (!a_ldltAnalyzed) {
            a_ldlt->analyzePattern(a_real);
            a_ldltAnalyzed = true;
        }
        a_ldlt->factorize(a_real);
        if (a_ldlt->info() == Eigen::Success && (a_ldlt->vectorD().array() != 0).all()) {
            a_lu.reset();
            a_analyzed = false;
            return true;
        }
        a_ldlt.reset();
        a_ldltAnalyzed = false;
    } else {
        a_ldlt.reset();
        a_ldltAnalyzed = false;
    }
    if (!a_lu) {
        a_lu = std::make_unique<Eigen::SparseLU<Matrix>>();
        a_analyzed = false;
    }
    if (!a_analyzed) {
        a_lu->analyzePattern(a_A);
        a_analyzed = true;
    }
    a_lu->factorize(a_A);
    if (a_lu->info() != Eigen::Success) {
        *error = tr("%1: the matrix is singular - %2").arg(name, QString::fromStdString(a_lu->lastErrorMessage()));
        a_lu.reset();
        a_analyzed = false;
        return false;
    }
    return true;
}

bool ComplexSystem::solve(const Eigen::VectorXcd& rhs, Eigen::VectorXcd& x, const QString& name, QString* error) const
{
    if (a_ldlt) {
        Eigen::VectorXd r(2 * a_n);
        for (int i = 0; i < a_n; ++i) {
            r[2 * i] = rhs[i].real();
            r[2 * i + 1] = -rhs[i].imag();
        }
        const Eigen::VectorXd y = a_ldlt->solve(r);
        x.resize(a_n);
        for (int i = 0; i < a_n; ++i) x[i] = std::complex<double>(y[2 * i], y[2 * i + 1]);
        if (!x.allFinite()) {
            *error = tr("%1: the solution is not finite").arg(name);
            return false;
        }
        return true;
    }
    if (!a_lu) {
        *error = tr("%1: nothing is factorized").arg(name);
        return false;
    }
    x = a_lu->solve(rhs);
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

QString Problem::givenProperty(const Node& m, const QString& key, const Scope* scope)
{
    // Its own, else its library entry's ({"material": "FR-4"} in a file).
    std::optional<MaterialEntry> entry;
    if (!m.text(QStringLiteral("library")).isEmpty()) entry = findMaterial(m.text(QStringLiteral("library")));
    auto own = [&](const QString& k) {
        const QString t = m.text(k);
        return t.trimmed().isEmpty() && entry ? entry->properties.value(k).toString() : t;
    };
    QString text = own(key);
    if (text.trimmed().isEmpty() && key == QLatin1String("Eg")) {
        // A gap wavelength's (the "1.3Q" quaternary's 1.3 µm): hc/λg.
        const QString lambda = own(QStringLiteral("lambdag"));
        const Scope none;
        if (!lambda.trimmed().isEmpty())
            if (const std::optional<double> l = evaluateConstant(lambda, scope ? *scope : none, Dim::length()); l && *l > 0)
                text = formatNumber(6.62607015e-34 * 299792458.0 / *l / 1.602176634e-19, 10) + QStringLiteral("[eV]");
    }
    return text;
}

QString Problem::materialProperty(const Node& m, const QString& key, const Scope* scope)
{
    QString text = givenProperty(m, key, scope);
    if (!text.trimmed().isEmpty() || key == QLatin1String("compound") || key == QLatin1String("x") || key == QLatin1String("y")) return text;
    // A III-V alloy's: from its compound at its composition.
    std::optional<MaterialEntry> entry;
    if (!m.text(QStringLiteral("library")).isEmpty()) entry = findMaterial(m.text(QStringLiteral("library")));
    auto own = [&](const QString& k) {
        const QString t = m.text(k);
        return t.trimmed().isEmpty() && entry ? entry->properties.value(k).toString() : t;
    };
    const QString compound = own(QStringLiteral("compound"));
    if (compound.trimmed().isEmpty()) return text;
    const Scope none;
    double xy[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        const QString t = own(i == 0 ? QStringLiteral("x") : QStringLiteral("y"));
        if (t.trimmed().isEmpty()) continue;
        const std::optional<double> v = evaluateConstant(t, scope ? *scope : none, Dim());
        if (!v) return text;
        xy[i] = *v;
    }
    return compoundProperty(compound, xy[0], xy[1], key, bandSetOf(own(QStringLiteral("bands"))));
}

QJsonValue materialValue(const Node& m, const QString& key)
{
    const QJsonValue v = m.value(key);
    if (v.isArray() && !v.toArray().isEmpty()) return v;
    if (v.isString() && !v.toString().trimmed().isEmpty()) return v;
    if (v.isDouble()) return v;
    if (!m.text(QStringLiteral("library")).isEmpty())
        if (const std::optional<MaterialEntry> entry = findMaterial(m.text(QStringLiteral("library"))))
            return entry->properties.value(key);
    return {};
}

std::unique_ptr<Problem> makeScalarProblem(Solution& solution, int field);
std::unique_ptr<Problem> makeElasticProblem(Solution& solution, int field);
std::unique_ptr<Problem> makeMagneticProblem(Solution& solution, int field);
std::unique_ptr<Problem> makeModeProblem(Solution& solution, int field);
std::unique_ptr<Problem> makeWaveProblem(Solution& solution, int field);
std::unique_ptr<Problem> makeSemiconductorProblem(Solution& solution, int field);
std::unique_ptr<Problem> makeRayProblem(Solution& solution, int field);
std::unique_ptr<Problem> makeQuantumProblem(Solution& solution, int field);
std::unique_ptr<Problem> makePdeProblem(Solution& solution, int field);

std::unique_ptr<Problem> makeProblem(Solution& solution, int field)
{
    const QString& type = solution.fields()[std::size_t(field)].type;
    if (type == QLatin1String("semiconductor")) return makeSemiconductorProblem(solution, field);
    if (type == QLatin1String("rays")) return makeRayProblem(solution, field);
    if (type == QLatin1String("quantum")) return makeQuantumProblem(solution, field);
    if (type == QLatin1String("pde")) return makePdeProblem(solution, field);
    if (type == QLatin1String("solid")) return makeElasticProblem(solution, field);
    if (type == QLatin1String("magnetic")) return makeMagneticProblem(solution, field);
    if (type == QLatin1String("modes")) return makeModeProblem(solution, field);
    if (type == QLatin1String("waves")) return makeWaveProblem(solution, field);
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
    bool line = false;          // (quantum, pde: 1D along a line)
    int pdeVars = 0;            // (pde: its variables, before their rates)
};

FieldInfo infoOf(const Node& p, bool axisymmetric)
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
    } else if (p.type == QLatin1String("magnetic")) {
        // The magnetic vector potential out of the plane (round the axis).
        f.variable = axisymmetric ? QStringLiteral("Aphi") : QStringLiteral("Az");
        f.dim = Dim::of(1, 1, -2, -1);
    } else if (p.type == QLatin1String("modes")) {
        f.variable = QStringLiteral("E");
        f.dim = Dim::of(1, 1, -3, -1);
    } else if (p.type == QLatin1String("rays")) {
        // What the rays leave in the media: a power density.
        f.variable = QStringLiteral("Q");
        f.dim = Dim::of(-1, 1, -3);
    } else if (p.type == QLatin1String("quantum")) {
        // Each carrier's wavefunction (of the state selected).
        f.variable = QStringLiteral("psi");
        f.line = p.text(QStringLiteral("space")) != QLatin1String("2d");
    } else if (p.type == QLatin1String("pde")) {
        // Its variables (of their unit), and their rates when it is of the
        // second order in time.
        const QStringList names = detail::pdeVariables(p);
        f.variable = names.front();
        const QString unit = p.text(QStringLiteral("unit")).trimmed();
        if (!unit.isEmpty() && unit != QLatin1String("1")) {
            const Expression e = Expression::compile(QStringLiteral("1[%1]").arg(unit), Scope());
            if (e.isValid()) f.dim = e.dim();
        }
        f.line = p.text(QStringLiteral("space")) == QLatin1String("1d");
        f.pdeVars = int(names.size());
    } else if (p.type == QLatin1String("semiconductor")) {
        // The potential; the carriers and the doping with it.
        f.variable = QStringLiteral("V");
        f.dim = Dim::voltage();
    } else if (p.type == QLatin1String("waves")) {
        // E or H out of the plane.
        const bool h = p.text(QStringLiteral("polarization")) == QLatin1String("hz");
        f.variable = h ? QStringLiteral("Hz") : QStringLiteral("Ez");
        f.dim = h ? Dim::of(-1, 0, 0, 1) : Dim::of(1, 1, -3, -1);
    } else {
        f.variable = QStringLiteral("V");
        f.dim = Dim::voltage();
    }
    if (p.type == QLatin1String("solid")) f.components = QStringList{QStringLiteral("u"), QStringLiteral("v")};
    // (Its real and imaginary parts: in the frequency domain it is complex.)
    else if (p.type == QLatin1String("magnetic") || p.type == QLatin1String("waves"))
        f.components = QStringList{f.variable, f.variable + QStringLiteral("_im")};
    else if (p.type == QLatin1String("modes")) f.components = QStringList{QStringLiteral("Et"), QStringLiteral("Ez")};
    else if (p.type == QLatin1String("semiconductor"))
        f.components = QStringList{QStringLiteral("V"), QStringLiteral("n"), QStringLiteral("p"), QStringLiteral("Nd"), QStringLiteral("Na")};
    else if (p.type == QLatin1String("quantum"))
        f.components = QStringList{QStringLiteral("psi_e"), QStringLiteral("psi_hh"), QStringLiteral("psi_lh"), QStringLiteral("n"), QStringLiteral("p")};
    else if (p.type == QLatin1String("pde")) {
        f.components = detail::pdeVariables(p);
        if (detail::pdeSecondOrder(p))
            for (int k = 0, n = int(f.components.size()); k < n; ++k) f.components << f.components[k] + QLatin1Char('t');
    }
    else f.components = QStringList{f.variable};
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
        const FieldInfo info = infoOf(*p, a_axisymmetric);
        Field f;
        f.tag = info.tag;
        f.type = info.type;
        f.variable = info.variable;
        f.components = info.components;
        f.dim = info.dim;
        f.initial = p->type == QLatin1String("heat") ? 293.15 : 0.0;
        // (A semiconductor's elements are linear: Scharfetter and Gummel's
        // currents along their edges.)
        const int order = p->text(QStringLiteral("order")) == QLatin1String("1") || p->type == QLatin1String("semiconductor")
                                  || p->type == QLatin1String("rays")
                              ? 1
                              : 2;
        auto it = spaces.find(order);
        if (it == spaces.end()) it = spaces.emplace(order, std::make_shared<Space>(a_mesh, order)).first;
        f.space = it->second;
        f.values.assign(std::size_t(f.space->size()) * std::size_t(f.count()), f.initial);
        if (p->type == QLatin1String("modes")) {
            // Et on edge elements of the order, Ez on the nodes: each complex.
            f.edges = std::make_shared<EdgeSpace>(a_mesh, order);
            f.values.assign(std::size_t(2 * f.edges->size() + 2 * f.space->size()), 0.0);
            f.extra.assign(3, 0.0);
        }
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
    d.info = {name, dim, description, QString()};
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

constexpr double Mu0 = 1.25663706127e-6;

/// A magnetic field at a point: B and H (each component's real and
/// imaginary parts), the current density out of the plane (round the
/// axis), what is conducted of it, the energy and loss densities (time
/// averages in the frequency domain).
struct MagneticState {
    double B[2][2] = {{0, 0}, {0, 0}};   // [x (r), y (z)][re, im]
    double H[2][2] = {{0, 0}, {0, 0}};
    double J[2] = {0, 0};
    double Jc[2] = {0, 0};
    double sigma = 0;
    double mur = NaN;
    double w = 0;
    double q = 0;
};

MagneticState magneticAt(PointEvaluator& e, int f, bool axisymmetric)
{
    MagneticState st;
    double a[2], ax[2], ay[2];
    for (int c = 0; c < 2; ++c) e.component(f, c, a[c], ax[c], ay[c]);
    if (!std::isfinite(a[0])) {
        for (auto& b : st.B) b[0] = b[1] = NaN;
        for (auto& h : st.H) h[0] = h[1] = NaN;
        st.J[0] = st.J[1] = st.Jc[0] = st.Jc[1] = st.w = st.q = NaN;
        return st;
    }
    const double r = axisymmetric ? e.point().x() : 1.0;
    for (int c = 0; c < 2; ++c) {
        if (axisymmetric) {
            // B = curl(A φ): (-∂A/∂z, ∂A/∂r + A/r)
            st.B[0][c] = -ay[c];
            st.B[1][c] = ax[c] + (r > 0 ? a[c] / r : ax[c]);
        } else {
            st.B[0][c] = ay[c];
            st.B[1][c] = -ax[c];
        }
    }
    const Solution& s = e.solution();
    const int dom = e.domain();
    const Solution::Coefficients& coef = s.coefficients()[std::size_t(f)];
    const BHCurve* curve = dom >= 0 && dom < int(coef.curves.size()) ? coef.curves[std::size_t(dom)].get() : nullptr;
    auto orZero = [](double v) { return std::isfinite(v) ? v : 0.0; };
    const double brx = orZero(e.coefficient(f, QStringLiteral("Brx"))), bry = orZero(e.coefficient(f, QStringLiteral("Bry")));
    // (Time averages of a field that changes: none at 0 Hz.)
    const bool harmonic = s.frequencyDomain && s.frequency() > 0;
    double nu = NaN;
    if (curve) {
        const double b = std::sqrt(st.B[0][0] * st.B[0][0] + st.B[1][0] * st.B[1][0] + st.B[0][1] * st.B[0][1] + st.B[1][1] * st.B[1][1]);
        nu = curve->nu(b);
        st.mur = 1 / (Mu0 * nu);
        st.w = harmonic ? 0.25 * nu * b * b : curve->energy(b);
    } else {
        st.mur = e.coefficient(f, QString());
        nu = 1 / (Mu0 * st.mur);
        const double dx = st.B[0][0] - brx, dy = st.B[1][0] - bry;
        st.w = harmonic ? 0.25 * nu * (st.B[0][0] * st.B[0][0] + st.B[1][0] * st.B[1][0] + st.B[0][1] * st.B[0][1] + st.B[1][1] * st.B[1][1])
                        : 0.5 * nu * (dx * dx + dy * dy);
    }
    st.H[0][0] = nu * (st.B[0][0] - brx);
    st.H[1][0] = nu * (st.B[1][0] - bry);
    st.H[0][1] = nu * st.B[0][1];
    st.H[1][1] = nu * st.B[1][1];
    st.sigma = orZero(e.coefficient(f, QStringLiteral("sigma")));
    const double je = orZero(e.coefficient(f, QStringLiteral("Je")));
    // A conductor's field from its voltage: U along the plane's normal (V/m),
    // round the axis U/(2πr) (U its loop voltage); and -jωA.
    double ure = 0, uim = 0;
    const double coil = e.coefficient(f, QStringLiteral("coil"));
    if (std::isfinite(coil) && coil >= 0 && coil < double(s.fields()[std::size_t(f)].terminals.size())) {
        const Field::Terminal& t = s.fields()[std::size_t(f)].terminals[std::size_t(coil)];
        ure = t.potential;
        uim = t.potentialIm;
    }
    const double g = axisymmetric ? (r > 0 ? 1 / (2 * M_PI * r) : 0.0) : 1.0;
    const double omega = 2 * M_PI * s.frequency();
    const double ere = g * ure + omega * a[1], eim = g * uim - omega * a[0];
    st.Jc[0] = st.sigma * ere;
    st.Jc[1] = st.sigma * eim;
    st.J[0] = je + st.Jc[0];
    st.J[1] = st.Jc[1];
    st.q = st.sigma * (ere * ere + eim * eim) * (harmonic ? 0.5 : 1.0);
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
    define(QStringLiteral("freq"), Dim::of(0, 0, -1), tr("frequency (in the frequency domain; 0 at rest)"), K::Frequency);
    define(QStringLiteral("dom"), Dim(), tr("the domain's number"), K::DomainNumber);
    define(QStringLiteral("h"), Dim::length(), tr("the mesh element's size"), K::Size);
    // A wavelength, of what waves and rays are at.
    bool optical = false;
    for (const FieldInfo& fi : fields)
        optical = optical || fi.type == QLatin1String("rays") || fi.type == QLatin1String("modes") || fi.type == QLatin1String("waves")
                  || fi.type == QLatin1String("quantum");
    if (optical)
        define(QStringLiteral("lambda0"), Dim::length(), tr("free-space wavelength: c0/freq (a ray's, a mode's)"), K::Derived, -1, [](PointEvaluator& e) {
            const double f = e.frequency();
            return f > 0 ? 299792458.0 / f : NaN;
        });
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
        if (field.type == QLatin1String("modes") || field.type == QLatin1String("waves")) {
            const bool modes = field.type == QLatin1String("modes");
            const QString im = QStringLiteral("_im");
            const Dim eDim = Dim::of(1, 1, -3, -1), hDim = Dim::of(-1, 0, 0, 1), sDim = Dim::of(0, 1, -3);
            using FieldsAt = std::function<void(PointEvaluator&, std::complex<double>*, std::complex<double>*, bool*)>;
            FieldsAt at;
            if (modes)
                at = [f](PointEvaluator& e, std::complex<double>* E, std::complex<double>* H, bool* ok) {
                    const detail::ModeFields m = detail::modeFieldsAt(e, f);
                    for (int k = 0; k < 3; ++k) {
                        E[k] = m.E[k];
                        H[k] = m.H[k];
                    }
                    *ok = m.defined;
                };
            else
                at = [f](PointEvaluator& e, std::complex<double>* E, std::complex<double>* H, bool* ok) {
                    const detail::WaveFields w = detail::waveFieldsAt(e, f);
                    for (int k = 0; k < 3; ++k) {
                        E[k] = w.E[k];
                        H[k] = w.H[k];
                    }
                    *ok = w.defined;
                };
            const char* axes[3] = {"x", "y", "z"};
            for (int k = 0; k < 3; ++k) {
                const QString c = QString::fromLatin1(axes[k]);
                for (int which = 0; which < 2; ++which) {
                    const bool e = which == 0;
                    const QString name = p + (e ? QStringLiteral("E") : QStringLiteral("H")) + c;
                    auto part = [at, k, e](PointEvaluator& ev, bool imag) {
                        std::complex<double> E[3], H[3];
                        bool ok = false;
                        at(ev, E, H, &ok);
                        if (!ok) return NaN;
                        const std::complex<double> v = e ? E[k] : H[k];
                        return imag ? v.imag() : v.real();
                    };
                    define(name, e ? eDim : hDim,
                           tr("%1 field, %2 component (its real part: at a phase of 0)").arg(e ? tr("electric") : tr("magnetic"), c), K::Derived, f,
                           [part](PointEvaluator& ev) { return part(ev, false); });
                    define(name + im, e ? eDim : hDim, tr("%1 field, %2 component, imaginary part").arg(e ? tr("electric") : tr("magnetic"), c),
                           K::Derived, f, [part](PointEvaluator& ev) { return part(ev, true); });
                }
            }
            for (int which = 0; which < 2; ++which) {
                const bool e = which == 0;
                define(p + (e ? QStringLiteral("normE") : QStringLiteral("normH")), e ? eDim : hDim,
                       tr("%1 field, norm (its amplitude)").arg(e ? tr("electric") : tr("magnetic")), K::Derived, f, [at, e](PointEvaluator& ev) {
                           std::complex<double> E[3], H[3];
                           bool ok = false;
                           at(ev, E, H, &ok);
                           if (!ok) return NaN;
                           const std::complex<double>* v = e ? E : H;
                           return std::sqrt(std::norm(v[0]) + std::norm(v[1]) + std::norm(v[2]));
                       });
            }
            // The power flow, its time average: ½ Re(E × H*).
            for (int k = 0; k < 3; ++k) {
                const QString c = QString::fromLatin1(axes[k]);
                define(p + QStringLiteral("S") + c, sDim, tr("power flow, %1 component (a time average)").arg(c), K::Derived, f,
                       [at, k](PointEvaluator& ev) {
                           std::complex<double> E[3], H[3];
                           bool ok = false;
                           at(ev, E, H, &ok);
                           if (!ok) return NaN;
                           const int a = (k + 1) % 3, b = (k + 2) % 3;
                           return 0.5 * (E[a] * std::conj(H[b]) - E[b] * std::conj(H[a])).real();
                       });
            }
            define(p + QStringLiteral("normS"), sDim, tr("power flow, norm"), K::Derived, f, [at](PointEvaluator& ev) {
                std::complex<double> E[3], H[3];
                bool ok = false;
                at(ev, E, H, &ok);
                if (!ok) return NaN;
                double s2 = 0;
                for (int k = 0; k < 3; ++k) {
                    const int a = (k + 1) % 3, b = (k + 2) % 3;
                    const double v = 0.5 * (E[a] * std::conj(H[b]) - E[b] * std::conj(H[a])).real();
                    s2 += v * v;
                }
                return std::sqrt(s2);
            });
            if (modes) {
                // What a mode carrying 1 W absorbs, per volume (W/m³): all of it,
                // ½ ω ε0 Σ -Im εk |Ek|²; across a gap - the media's κ and a direct
                // gap's, the electron-hole pairs a photodiode collects -, ω ε0 n κ |E|².
                const Dim qDim = Dim::of(-1, 1, -3), perMetre = Dim::of(-1);   // (W/m³)
                define(p + QStringLiteral("Qh"), qDim, tr("power absorbed per volume, the mode carrying 1 W"), K::Derived, f, [f](PointEvaluator& e) {
                    const detail::MediumAt m = detail::modeMediumAt(e, f);
                    const detail::ModeFields mf = detail::modeFieldsAt(e, f);
                    if (!m.defined || !mf.defined) return NaN;
                    double q = 0;
                    for (int k = 0; k < 3; ++k) q += m.lossEps[k] * std::norm(mf.E[k]);
                    return 0.5 * m.omega * detail::Eps0 * q;
                });
                define(p + QStringLiteral("Qgap"), qDim,
                       tr("power absorbed per volume across a gap (the media's κ and a direct gap's, not the free carriers'), the mode "
                          "carrying 1 W: the electron-hole pairs made, times ħω"),
                       K::Derived, f, [f](PointEvaluator& e) {
                           const detail::MediumAt m = detail::modeMediumAt(e, f);
                           const detail::ModeFields mf = detail::modeFieldsAt(e, f);
                           if (!m.defined || !mf.defined) return NaN;
                           return m.omega * detail::Eps0 * m.n * m.kappaGap * (std::norm(mf.E[0]) + std::norm(mf.E[1]) + std::norm(mf.E[2]));
                       });
                define(p + QStringLiteral("alphagap"), perMetre, tr("absorption coefficient across a gap, 4πκ/λ (the power's)"), K::Derived, f,
                       [f](PointEvaluator& e) {
                           const detail::MediumAt m = detail::modeMediumAt(e, f);
                           return m.defined ? 2 * m.omega / 299792458.0 * m.kappaGap : NaN;
                       });
                define(p + QStringLiteral("dalphaEA"), perMetre,
                       tr("what the field adds to a direct gap's absorption coefficient (the Franz-Keldysh effect)"), K::Derived, f,
                       [f](PointEvaluator& e) {
                           const detail::MediumAt m = detail::modeMediumAt(e, f);
                           return m.defined ? 2 * m.omega / 299792458.0 * (m.kappaEA - m.kappaEA0) : NaN;
                       });
            }
            if (!modes) {
                component(p + field.variable, field.dim, tr("the field out of the plane%1").arg(tr(" (its real part)")), K::Value, f, 0);
                component(p + field.variable + im, field.dim, tr("the field out of the plane, its imaginary part"), K::Value, f, 1);
            }
            coefficient(p + QStringLiteral("n"), Dim(), tr("refractive index (as given)"), f, QString());
            continue;
        }
        if (field.type == QLatin1String("rays")) {
            component(p + QStringLiteral("Q"), Dim::of(-1, 1, -3), tr("power absorbed from the rays, per volume"), K::Value, f, 0);
            coefficient(p + QStringLiteral("n"), Dim(), tr("refractive index"), f, QString());
            continue;
        }
        if (field.type == QLatin1String("pde")) {
            // Each variable (and rate) by its own name and with the tag; its
            // gradient; along a line, from the line's values.
            const Dim length = Dim::length(), time = Dim::of(0, 0, 1);
            for (int c = 0; c < int(field.components.size()); ++c) {
                const QString name = field.components[c];
                const bool rate = c >= field.pdeVars;
                const Dim dim = rate ? field.dim / time : field.dim;
                const QString what = rate ? tr("the rate of %1, ∂%1/∂t").arg(field.components[c - field.pdeVars]) : tr("dependent variable %1").arg(name);
                if (field.line) {
                    auto at = [f, c](PointEvaluator& e, int part) {
                        double u = NaN, us = NaN;
                        detail::pdeLineAt(e, f, c, &u, &us);
                        if (part == 0) return u;
                        if (part == 3) return us;
                        // The gradient along the line's direction.
                        const Field& fl = e.solution().fields()[std::size_t(f)];
                        if (!fl.fixed || fl.fixed->size() < 5) return NaN;
                        const std::vector<double>& fx = *fl.fixed;
                        const double dx = fx[3] - fx[1], dy = fx[4] - fx[2], L = std::hypot(dx, dy);
                        return L > 0 ? us * (part == 1 ? dx : dy) / L : NaN;
                    };
                    for (const QString& n : {p + name, name}) {
                        define(n, dim, what, K::Derived, f, [at](PointEvaluator& e) { return at(e, 0); });
                        define(n + QLatin1Char('x'), dim / length, tr("%1's derivative in x").arg(name), K::Derived, f,
                               [at](PointEvaluator& e) { return at(e, 1); });
                        define(n + QLatin1Char('y'), dim / length, tr("%1's derivative in y").arg(name), K::Derived, f,
                               [at](PointEvaluator& e) { return at(e, 2); });
                        define(n + QLatin1Char('s'), dim / length, tr("%1's derivative along the line").arg(name), K::Derived, f,
                               [at](PointEvaluator& e) { return at(e, 3); });
                    }
                } else {
                    for (const QString& n : {p + name, name}) {
                        component(n, dim, what, K::Value, f, c);
                        component(n + QLatin1Char('x'), dim / length, tr("%1's derivative in x").arg(name), K::GradX, f, c);
                        component(n + QLatin1Char('y'), dim / length, tr("%1's derivative in y").arg(name), K::GradY, f, c);
                    }
                }
            }
            continue;
        }
        if (field.type == QLatin1String("quantum")) {
            const Dim energy = Dim::of(2, 1, -2);
            const Dim density = field.line ? Dim::of(-1) : axisymmetric ? Dim::of(-3) : Dim::of(-2);
            auto st = [f](PointEvaluator& e) { return detail::quantumAt(e, f); };
            const QString carriers[3] = {QStringLiteral("e"), QStringLiteral("hh"), QStringLiteral("lh")};
            const QString names[3] = {tr("electron"), tr("heavy hole"), tr("light hole")};
            for (int c = 0; c < 3; ++c) {
                const QString n = carriers[c];
                define(p + QStringLiteral("psi_") + n, Dim(), tr("the %1 state's wavefunction, its largest 1").arg(names[c]), K::Derived, f,
                       [st, c](PointEvaluator& e) {
                           const detail::QuantumState q = st(e);
                           return q.defined && q.peak[c] > 0 ? q.psi[c] / std::sqrt(q.peak[c]) : NaN;
                       });
                define(p + QStringLiteral("P_") + n, density, tr("the %1 state's probability density |ψ|²").arg(names[c]), K::Derived, f,
                       [st, c](PointEvaluator& e) {
                           const detail::QuantumState q = st(e);
                           return q.defined ? q.psi[c] * q.psi[c] : NaN;
                       });
                // A band diagram's picture: the state's level, its |ψ|² on it
                // (a hole's hanging down).
                define(p + QStringLiteral("wave_") + n, energy, tr("the %1 state's energy and its |ψ|² drawn on it").arg(names[c]), K::Derived,
                       f, [st, c](PointEvaluator& e) {
                           const detail::QuantumState q = st(e);
                           if (!q.defined || !(q.peak[c] > 0)) return NaN;
                           return q.E[c] + (c == 0 ? 1 : -1) * q.drawn * q.psi[c] * q.psi[c] / q.peak[c];
                       });
            }
            define(p + QStringLiteral("Ec"), energy, tr("conduction band edge"), K::Derived, f, [st](PointEvaluator& e) { return st(e).Ec; });
            define(p + QStringLiteral("Ehh"), energy, tr("heavy holes' band edge"), K::Derived, f, [st](PointEvaluator& e) { return st(e).Ehh; });
            define(p + QStringLiteral("Elh"), energy, tr("light holes' band edge"), K::Derived, f, [st](PointEvaluator& e) { return st(e).Elh; });
            define(p + QStringLiteral("Ev"), energy, tr("valence band edge: the heavy and light holes' higher"), K::Derived, f, [st](PointEvaluator& e) {
                const detail::QuantumState q = st(e);
                return std::max(q.Ehh, q.Elh);
            });
            define(p + QStringLiteral("me"), Dim(), tr("electron effective mass (m0)"), K::Derived, f, [st](PointEvaluator& e) { return st(e).me; });
            define(p + QStringLiteral("mhh"), Dim(), tr("heavy hole mass along the growth axis (m0)"), K::Derived, f,
                   [st](PointEvaluator& e) { return st(e).mhh; });
            define(p + QStringLiteral("mlh"), Dim(), tr("light hole mass along the growth axis (m0)"), K::Derived, f,
                   [st](PointEvaluator& e) { return st(e).mlh; });
            define(p + QStringLiteral("strain"), Dim(), tr("strain in the plane of the layers: (a_substrate - a)/a"), K::Derived, f,
                   [st](PointEvaluator& e) { return st(e).strain; });
            component(p + QStringLiteral("n"), Dim::of(-3), tr("electrons in the states found, filled to the Fermi level given"), K::Value, f, 3);
            component(p + QStringLiteral("p"), Dim::of(-3), tr("holes in the states found, filled to the Fermi level given"), K::Value, f, 4);
            continue;
        }
        if (field.type == QLatin1String("semiconductor")) {
            const Dim conc = Dim::of(-3), energy = Dim::of(2, 1, -2), rate = Dim::of(-3, 0, -1), mobility = Dim::of(0, -1, 2, 1);
            component(p + QStringLiteral("V"), Dim::voltage(), tr("electric potential (the intrinsic level's: Ei = -qV)"), K::Value, f, 0);
            component(QStringLiteral("V"), Dim::voltage(), tr("electric potential (%1)").arg(field.tag), K::Value, f, 0);
            component(p + QStringLiteral("Nd"), conc, tr("donor concentration"), K::Value, f, 3);
            component(p + QStringLiteral("Na"), conc, tr("acceptor concentration"), K::Value, f, 4);
            const int sd = component(p + QStringLiteral("Nd"), conc, QString(), K::Value, f, 3);
            const int sa = component(p + QStringLiteral("Na"), conc, QString(), K::Value, f, 4);
            define(p + QStringLiteral("Ndop"), conc, tr("net doping: donors less acceptors"), K::Derived, f,
                   [sd, sa](PointEvaluator& e) { return e.value(sd) - e.value(sa); });
            auto st = [f](PointEvaluator& e) { return detail::semiconductorAt(e, f); };
            auto semi = [st](double (*get)(const detail::SemiconductorState&)) {
                return [st, get](PointEvaluator& e) {
                    const detail::SemiconductorState s = st(e);
                    return s.semiconductor ? get(s) : NaN;
                };
            };
            define(p + QStringLiteral("n"), conc, tr("electron concentration"), K::Derived, f, semi([](const detail::SemiconductorState& s) { return s.n; }));
            define(p + QStringLiteral("p"), conc, tr("hole concentration"), K::Derived, f, semi([](const detail::SemiconductorState& s) { return s.p; }));
            define(p + QStringLiteral("ni"), conc, tr("intrinsic carrier concentration"), K::Derived, f,
                   semi([](const detail::SemiconductorState& s) { return s.ni; }));
            define(p + QStringLiteral("Ex"), efield, tr("electric field, x component"), K::Derived, f, [f](PointEvaluator& e) {
                double u = 0, gx = 0, gy = 0;
                e.component(f, 0, u, gx, gy);
                return -gx;
            });
            define(p + QStringLiteral("Ey"), efield, tr("electric field, y component"), K::Derived, f, [f](PointEvaluator& e) {
                double u = 0, gx = 0, gy = 0;
                e.component(f, 0, u, gx, gy);
                return -gy;
            });
            define(p + QStringLiteral("normE"), efield, tr("electric field, norm"), K::Derived, f, [f](PointEvaluator& e) {
                double u = 0, gx = 0, gy = 0;
                e.component(f, 0, u, gx, gy);
                return std::hypot(gx, gy);
            });
            define(p + QStringLiteral("rho"), Dim::of(-3, 0, 1, 1), tr("space charge density: q (p - n + Nd⁺ - Na⁻)"), K::Derived, f,
                   [st, sd, sa](PointEvaluator& e) {
                       const detail::SemiconductorState s = st(e);
                       return s.semiconductor ? 1.602176634e-19 * (s.p - s.n + s.donors(e.value(sd)) - s.acceptors(e.value(sa))) : 0.0;
                   });
            define(p + QStringLiteral("Ndi"), conc, tr("ionized donors Nd⁺ (all of them unless ionized in part)"), K::Derived, f,
                   [st, sd](PointEvaluator& e) {
                       const detail::SemiconductorState s = st(e);
                       return s.semiconductor ? s.donors(e.value(sd)) : NaN;
                   });
            define(p + QStringLiteral("Nai"), conc, tr("ionized acceptors Na⁻ (all of them unless ionized in part)"), K::Derived, f,
                   [st, sa](PointEvaluator& e) {
                       const detail::SemiconductorState s = st(e);
                       return s.semiconductor ? s.acceptors(e.value(sa)) : NaN;
                   });
            // Band energies (J; shown in eV): Ei = -q (V + δ) - δ each
            // semiconductor's offset across a heterojunction -, Ec = Ei + (Ec - Ei).
            const double q = 1.602176634e-19;
            define(p + QStringLiteral("Ei"), energy, tr("intrinsic level"), K::Derived, f, [st, q](PointEvaluator& e) {
                const detail::SemiconductorState s = st(e);
                return s.semiconductor ? -q * (s.V + s.delta) : NaN;
            });
            define(p + QStringLiteral("Ec"), energy, tr("conduction band edge"), K::Derived, f, [st, q](PointEvaluator& e) {
                const detail::SemiconductorState s = st(e);
                return s.semiconductor ? -q * (s.V + s.delta) + s.dEc : NaN;
            });
            define(p + QStringLiteral("Ev"), energy, tr("valence band edge"), K::Derived, f, [st, q](PointEvaluator& e) {
                const detail::SemiconductorState s = st(e);
                return s.semiconductor ? -q * (s.V + s.delta) + s.dEc - s.Eg : NaN;
            });
            define(p + QStringLiteral("Efn"), energy, tr("electrons' quasi-Fermi level"), K::Derived, f, [st, q](PointEvaluator& e) {
                const detail::SemiconductorState s = st(e);
                return s.semiconductor ? -q * s.phin : NaN;
            });
            define(p + QStringLiteral("Efp"), energy, tr("holes' quasi-Fermi level"), K::Derived, f, [st, q](PointEvaluator& e) {
                const detail::SemiconductorState s = st(e);
                return s.semiconductor ? -q * s.phip : NaN;
            });
            define(p + QStringLiteral("phin"), Dim::voltage(), tr("electrons' quasi-Fermi potential"), K::Derived, f, [st](PointEvaluator& e) {
                const detail::SemiconductorState s = st(e);
                return s.semiconductor ? s.phin : NaN;
            });
            define(p + QStringLiteral("phip"), Dim::voltage(), tr("holes' quasi-Fermi potential"), K::Derived, f, [st](PointEvaluator& e) {
                const detail::SemiconductorState s = st(e);
                return s.semiconductor ? s.phip : NaN;
            });
            struct Cur {
                const char* name;
                QString what;
                int kind;   // 0 electrons', 1 holes', 2 both
                int axis;   // 0 x, 1 y, 2 norm
            };
            const Cur currents[] = {
                {"Jnx", tr("electron current density, x component"), 0, 0}, {"Jny", tr("electron current density, y component"), 0, 1},
                {"normJn", tr("electron current density, norm"), 0, 2},     {"Jpx", tr("hole current density, x component"), 1, 0},
                {"Jpy", tr("hole current density, y component"), 1, 1},     {"normJp", tr("hole current density, norm"), 1, 2},
                {"Jx", tr("current density, x component"), 2, 0},           {"Jy", tr("current density, y component"), 2, 1},
                {"normJ", tr("current density, norm"), 2, 2},
            };
            for (const Cur& c : currents) {
                const int kind = c.kind, axis = c.axis;
                define(p + QString::fromLatin1(c.name), jfield, c.what, K::Derived, f, [st, kind, axis](PointEvaluator& e) {
                    const detail::SemiconductorState s = st(e);
                    if (!s.semiconductor) return NaN;
                    double v[2];
                    for (int k = 0; k < 2; ++k) v[k] = kind == 0 ? s.Jn[k] : kind == 1 ? s.Jp[k] : s.Jn[k] + s.Jp[k];
                    return axis == 2 ? std::hypot(v[0], v[1]) : v[axis];
                });
            }
            define(p + QStringLiteral("R"), rate, tr("net recombination rate"), K::Derived, f, semi([](const detail::SemiconductorState& s) { return s.R; }));
            define(p + QStringLiteral("mun"), mobility, tr("electron mobility"), K::Derived, f, semi([](const detail::SemiconductorState& s) { return s.mun; }));
            // The heat the carriers leave: their Joule heat as solved (each
            // edge's current times its quasi-Fermi potential's drop, by
            // triangle) and the pairs' recombination's, q R (φp - φn) - a
            // heat physics' source (the generation's share not in it).
            define(p + QStringLiteral("H"), Dim::of(-1, 1, -3), tr("heat the carriers leave: their Joule heat, J·∇φ of each, and q R (φp - φn)"),
                   K::Derived, f, semi([](const detail::SemiconductorState& s) { return s.joule + 1.602176634e-19 * s.R * (s.phip - s.phin); }));
            define(p + QStringLiteral("mup"), mobility, tr("hole mobility"), K::Derived, f, semi([](const detail::SemiconductorState& s) { return s.mup; }));
            coefficient(p + QStringLiteral("epsr"), Dim(), tr("relative permittivity"), f, QString());
            continue;
        }
        if (field.type == QLatin1String("magnetic")) {
            const QString A = field.variable;
            const QString im = QStringLiteral("_im");
            const QString inFrequency = tr(" (in the frequency domain, its real part)");
            const Dim bDim = Dim::of(0, 1, -2, -1), hDim = Dim::of(-1, 0, 0, 1), jDim = Dim::of(-2, 0, 0, 1);
            const QString along = axisymmetric ? tr("round the axis") : tr("out of the plane");
            component(p + A, field.dim, tr("magnetic vector potential, %1%2").arg(along, inFrequency), K::Value, f, 0);
            component(A, field.dim, tr("magnetic vector potential (%1)").arg(field.tag), K::Value, f, 0);
            component(p + A + im, field.dim, tr("magnetic vector potential, its imaginary part"), K::Value, f, 1);
            const bool axi = axisymmetric;
            auto st = [f, axi](PointEvaluator& e) { return magneticAt(e, f, axi); };
            const QString b0 = axi ? QStringLiteral("r") : QStringLiteral("x"), b1 = axi ? QStringLiteral("z") : QStringLiteral("y");
            for (int k = 0; k < 2; ++k) {
                const QString c = k == 0 ? b0 : b1;
                define(p + QStringLiteral("B") + c, bDim, tr("magnetic flux density, %1 component").arg(c), K::Derived, f,
                       [st, k](PointEvaluator& e) { return st(e).B[k][0]; });
                define(p + QStringLiteral("B") + c + im, bDim, tr("magnetic flux density, %1 component, imaginary part").arg(c), K::Derived, f,
                       [st, k](PointEvaluator& e) { return st(e).B[k][1]; });
                define(p + QStringLiteral("H") + c, hDim, tr("magnetic field, %1 component").arg(c), K::Derived, f,
                       [st, k](PointEvaluator& e) { return st(e).H[k][0]; });
                define(p + QStringLiteral("H") + c + im, hDim, tr("magnetic field, %1 component, imaginary part").arg(c), K::Derived, f,
                       [st, k](PointEvaluator& e) { return st(e).H[k][1]; });
            }
            define(p + QStringLiteral("normB"), bDim, tr("magnetic flux density, norm (its amplitude in the frequency domain)"), K::Derived, f,
                   [st](PointEvaluator& e) {
                       const MagneticState m = st(e);
                       return std::sqrt(m.B[0][0] * m.B[0][0] + m.B[0][1] * m.B[0][1] + m.B[1][0] * m.B[1][0] + m.B[1][1] * m.B[1][1]);
                   });
            define(p + QStringLiteral("normH"), hDim, tr("magnetic field, norm (its amplitude)"), K::Derived, f, [st](PointEvaluator& e) {
                const MagneticState m = st(e);
                return std::sqrt(m.H[0][0] * m.H[0][0] + m.H[0][1] * m.H[0][1] + m.H[1][0] * m.H[1][0] + m.H[1][1] * m.H[1][1]);
            });
            const QString J = axi ? QStringLiteral("Jphi") : QStringLiteral("Jz");
            define(p + J, jDim, tr("current density, %1: the sources' and what is induced%2").arg(along, inFrequency), K::Derived, f,
                   [st](PointEvaluator& e) { return st(e).J[0]; });
            define(p + J + im, jDim, tr("current density, its imaginary part"), K::Derived, f, [st](PointEvaluator& e) { return st(e).J[1]; });
            define(p + QStringLiteral("normJ"), jDim, tr("current density, its amplitude"), K::Derived, f,
                   [st](PointEvaluator& e) { return std::hypot(st(e).J[0], st(e).J[1]); });
            define(p + QStringLiteral("Jc"), jDim, tr("conducted current density (σ E): eddy currents, a conductor's own%1").arg(inFrequency),
                   K::Derived, f, [st](PointEvaluator& e) { return st(e).Jc[0]; });
            define(p + QStringLiteral("Qrh"), Dim::of(-1, 1, -3), tr("resistive losses (a time average in the frequency domain)"), K::Derived, f,
                   [st](PointEvaluator& e) { return st(e).q; });
            define(p + QStringLiteral("Wm"), Dim::of(-1, 1, -2), tr("magnetic energy density (a time average in the frequency domain)"), K::Derived,
                   f, [st](PointEvaluator& e) { return st(e).w; });
            define(p + QStringLiteral("mur"), Dim(), tr("relative permeability (B/µ0H on a B-H curve)"), K::Derived, f,
                   [st](PointEvaluator& e) { return st(e).mur; });
            coefficient(p + QStringLiteral("sigma"), Dim::of(-3, -1, 3, 2), tr("electrical conductivity (0 in a coil of turns)"), f,
                        QStringLiteral("sigma"));
            coefficient(p + QStringLiteral("Je"), jDim, tr("external current density (a coil of turns' too)"), f, QStringLiteral("Je"));
            if (axi) {
                // The flux through the circle of radius r about the axis: its
                // contours are the field lines.
                const int sa = component(p + A, field.dim, QString(), K::Value, f, 0);
                define(p + QStringLiteral("psi"), Dim::of(2, 1, -2, -1), tr("magnetic flux through the circle of radius r (2πr Aφ): its contours are field lines"),
                       K::Derived, f, [sa](PointEvaluator& e) { return 2 * M_PI * e.point().x() * e.value(sa); });
            }
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
                coefficient(p + QStringLiteral("tand"), Dim(), tr("loss tangent (its material's)"), f, QStringLiteral("tand"));
                coefficient(p + QStringLiteral("sigma"), Dim::of(-3, -1, 3, 2), tr("electrical conductivity (its material's)"), f,
                            QStringLiteral("sigma"));
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
        if (p->enabled) out.push_back(infoOf(*p, axisymmetricModel(model)));
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
                             out << Variable{name, dim, description, QString()};
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
            if (p->tag == f.tag) {
                info.planeStress = p->text(QStringLiteral("model")) == QLatin1String("planestress");
                info.line = (p->type == QLatin1String("quantum") && p->text(QStringLiteral("space")) != QLatin1String("2d"))
                            || (p->type == QLatin1String("pde") && p->text(QStringLiteral("space")) == QLatin1String("1d"));
                if (p->type == QLatin1String("pde")) info.pdeVars = int(detail::pdeVariables(*p).size());
            }
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

void Solution::setGlobal(const QString& name, double value, const Dim& dim, const QString& description, const QString& unit)
{
    a_globalScope->setConstant(name, value, dim);
    // A unit of its dimension (eV, 1/nm) is where it is shown, the value
    // converted; one its dimension cannot say (dB/m) is the value's own.
    QString own, display;
    if (!unit.isEmpty()) {
        const std::optional<Unit> parsed = parseUnit(unit);
        if (parsed && parsed->dim == dim) display = unit;
        else own = unit;
    }
    for (Variable& v : a_globals)
        if (v.name == name) {
            v.dim = dim;
            if (!description.isEmpty()) v.description = description;
            v.unit = own;
            v.display = display;
            return;
        }
    a_globals << Variable{name, dim, description, own, display};
}

QString Solution::globalText(const QString& name) const
{
    for (const Variable& v : a_globals)
        if (v.name == name) {
            const double value = a_globalScope->constant(name)->value;
            if (!v.display.isEmpty())
                if (const std::optional<Unit> u = parseUnit(v.display))
                    return formatNumber((value - u->offset) / u->scale, 4) + QLatin1Char(' ') + v.display;
            return v.unit.isEmpty() ? formatQuantity(value, v.dim) : formatNumber(value, 4) + QLatin1Char(' ') + v.unit;
        }
    return {};
}

std::optional<double> Solution::global(const QString& name) const
{
    for (const Variable& v : a_globals)
        if (v.name == name) return a_globalScope->constant(name)->value;
    return std::nullopt;
}

QString Solution::snapshotText(int i) const
{
    const double v = snapshotValue(i);
    if (frequencyDomain && photonAxis == 1)
        return QStringLiteral("E = ") + formatQuantity(6.62607015e-34 * v / 1.602176634e-19, Dim()) + QStringLiteral(" eV");
    if (frequencyDomain && photonAxis == 2) return QStringLiteral("λ = ") + formatQuantity(v > 0 ? 299792458.0 / v : 0, Dim::length());
    if (frequencyDomain && photonAxis == 3) return QStringLiteral("k = ") + formatNumber(v * 1e-9, 4) + QStringLiteral(" /nm");
    return snapshotSymbol() + QStringLiteral(" = ") + formatQuantity(v, snapshotDim());
}

void Solution::addSnapshot()
{
    Snapshot s;
    s.time = a_time;
    s.frequency = a_frequency;
    s.mode = a_mode;
    for (const Field& f : a_fields) {
        s.values.push_back(f.values);
        s.terminals.push_back(f.terminals);
        s.extra.push_back(f.extra);
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
        if (f < s.extra.size()) a_fields[f].extra = s.extra[f];
    }
    a_time = s.time;
    a_frequency = s.frequency;
    a_mode = s.mode;
    for (int g = 0; g < int(a_globals.size()) && g < int(s.globals.size()); ++g)
        a_globalScope->setConstant(a_globals[g].name, s.globals[std::size_t(g)], a_globals[g].dim);
    a_selected = i;
}

void Solution::replaceLastSnapshot()
{
    if (a_snapshots.empty()) return;
    const double t = a_snapshots.back().time, f = a_snapshots.back().frequency;
    const int mode = a_snapshots.back().mode;
    a_snapshots.pop_back();
    const double nowT = a_time, nowF = a_frequency;
    const int nowM = a_mode;
    a_time = t;
    a_frequency = f;
    a_mode = mode;
    addSnapshot();
    a_time = nowT;
    a_frequency = nowF;
    a_mode = nowM;
}

int Solution::snapshotAtFrequency(double f) const
{
    int best = -1;
    double gap = INFINITY;
    for (int i = 0; i < int(a_snapshots.size()); ++i) {
        // (Nearest on a logarithmic scale: a list of decades.)
        const double v = a_snapshots[std::size_t(i)].frequency;
        const double d = v > 0 && f > 0 ? std::abs(std::log(v / f)) : std::abs(v - f);
        if (d < gap) {
            gap = d;
            best = i;
        }
    }
    return best;
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

int PointEvaluator::domain() const
{
    return a_t < 0 ? -1 : a_s.mesh().domain[std::size_t(a_t)];
}

void PointEvaluator::setFrequency(double f)
{
    a_frequency = f;
    std::fill(a_done.begin(), a_done.end(), 0);
}

double PointEvaluator::frequency() const
{
    return std::isnan(a_frequency) ? a_s.frequency() : a_frequency;
}

void PointEvaluator::perturb(int f, double du)
{
    if (f < 0 || f >= int(a_offset.size())) return;
    a_offset[std::size_t(f)] = du;
    std::fill(a_done.begin(), a_done.end(), 0);
}

void PointEvaluator::shift(int f, int c, int which, double delta)
{
    a_shift = delta == 0 ? Shift() : Shift{f, c, which, delta};
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
            for (std::size_t k = 0; k < std::size(c.u); ++k) c.u[k] = c.gx[k] = c.gy[k] = NaN;
        } else {
            Shape s;
            shapeAt(sp.order(), a_xi, a_eta, s);
            const Space::Map m = sp.map(a_t, a_xi, a_eta);
            const int* dofs = sp.dofs(a_t);
            const std::size_t n = std::size_t(sp.size());
            for (int k = 0; k < std::min(int(std::size(c.u)), field.count()); ++k) {
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
    if (comp < 0 || comp >= std::min(int(std::size(c.u)), field.count())) {
        u = gx = gy = NaN;
        return;
    }
    u = c.u[comp] + (comp == 0 ? a_offset[std::size_t(f)] : 0.0) + shiftOf(f, comp, 0);
    gx = c.gx[comp] + shiftOf(f, comp, 1);
    gy = c.gy[comp] + shiftOf(f, comp, 2);
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
    case K::Frequency: v = frequency(); break;
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
    static const QRegularExpression call(QStringLiteral("^\\s*(range|linspace|logspace)\\s*\\((.*)\\)\\s*$"));
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
            } else if (m.captured(1) == QLatin1String("logspace")) {
                // logspace(start, stop, count): evenly in their logarithms (the
                // values themselves, not their exponents: logspace(1[kHz], 1[GHz], 61))
                if (!agree(da, args[0], a) || !agree(db, args[1], b)) return std::nullopt;
                const int n = int(std::lround(c));
                if (n < 1 || n > 1000000) return fail(tr("%1: from 1 to a million values").arg(item));
                if (!(a > 0) || !(b > 0)) return fail(tr("%1: its start and stop must be more than 0").arg(item));
                for (int i = 0; i < n; ++i) out.push_back(n == 1 ? a : a * std::pow(b / a, double(i) / (n - 1)));
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

/// A step's progress told, the last of it kept: asked again whether to go
/// on - a long solve looks between its Newton steps - without moving it.
struct Telling {
    const std::function<bool(double, const QString&)>& to;
    double fraction = 0;
    QString what;
    bool operator()(double f, const QString& w)
    {
        fraction = f;
        what = w;
        return to(f, w);
    }
    bool stopped() { return !to(fraction, what); }
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
    // (A step without a Maximum iterations - a mode analysis's before it
    // had one - keeps the default: not 0 clamped to 1, a single Newton step.)
    if (st.node.kind() && st.node.kind()->property(QStringLiteral("maxiter")))
        st.maxIterations = std::clamp(st.node.integer(QStringLiteral("maxiter")), 1, 1000);
    st.options.maxIterations = st.maxIterations;
    return true;
}

/// The step's problems (their setup too: a semiconductor's way to its
/// contacts' voltages) asked whether to stop, as its progress is.
void cancelWith(Step& st, Telling& tell)
{
    for (auto& p : st.problems) p->setCancelled([&tell] { return tell.stopped(); });
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

/// Whether the problems \a subset of \a st converged; else why, for a step
/// whose modes, states or eigenvalues would be of a field not solved.
bool solvedBefore(const Step& st, const std::vector<int>& subset, QString* error)
{
    for (int i : subset)
        if (!st.problems[std::size_t(i)]->converged()) {
            detail::Problem& p = *st.problems[std::size_t(i)];
            *error = tr("%1: %2 did not converge in %3 Newton steps - what it solves would be of a field not solved: raise the step's "
                        "Maximum iterations, or solve it in a Stationary step before")
                         .arg(st.node.name(), p.node() ? p.node()->name() : p.field().tag)
                         .arg(st.maxIterations);
            return false;
        }
    return true;
}

bool stationaryStep(Solution& s, const Node& node, const std::function<bool(double, const QString&)>& told, QStringList* warnings, QString* error)
{
    Telling tell{told, 0, node.name()};
    Step st{s, node, {}, {}, {}, 50, 1e-6};
    if (!setupStep(st, error)) return false;
    cancelWith(st, tell);
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
        // Once more with the terminals (coils) swept.
        SolveOptions o = st.options;
        o.sweep = true;
        for (auto& p : st.problems)
            if (p->field().type == QLatin1String("electrostatics") || p->field().type == QLatin1String("currents")
                || p->field().type == QLatin1String("magnetic"))
                if (!p->solve(o, error)) return false;
    }
    for (auto& p : st.problems) p->setGlobals();
    // After a step in time or frequency: its last solution with these.
    if (s.snapshotCount() > 0) s.replaceLastSnapshot();
    return true;
}

/// The mode of mode analysis \a tag a semiconductor's Optical Generation
/// is lit by (its number: 1 the highest index), 1 when none asks.
int litMode(const Solution& s, const QString& tag)
{
    for (const Node* p : s.model().physics()) {
        if (!p->enabled || p->type != QLatin1String("semiconductor")) continue;
        for (const Node& f : p->children) {
            if (!f.enabled || f.type != QLatin1String("semiconductor/optical")) continue;
            QString mode = f.text(QStringLiteral("mode")).trimmed();
            if (mode.isEmpty()) mode = QStringLiteral("emw");
            if (mode == tag) return std::max(1, f.integer(QStringLiteral("number")));
        }
    }
    return 1;
}

bool modeStep(Solution& s, const Node& node, const std::function<bool(double, const QString&)>& told, QStringList* warnings, QString* error)
{
    Telling tell{told, 0, node.name()};
    Step st{s, node, {}, {}, {}, 50, 1e-6};
    if (!setupStep(st, error)) return false;
    cancelWith(st, tell);
    // Frequencies, or free-space wavelengths.
    Dim dim;
    QString why;
    const std::optional<std::vector<double>> listed = valueList(node.text(QStringLiteral("frequency")), s.parameters().scope, &dim, &why);
    if (!listed) {
        *error = tr("%1: the frequency: %2").arg(node.name(), why);
        return false;
    }
    std::vector<double> frequencies;
    for (double v : *listed) {
        const double f = dim == Dim::length() ? (v > 0 ? 299792458.0 / v : 0) : v;
        if (!(f > 0) || !std::isfinite(f)) {
            *error = tr("%1: a frequency (or wavelength) of %2: it must be more than 0").arg(node.name(), formatNumber(v));
            return false;
        }
        frequencies.push_back(f);
    }
    if (!dim.isNone() && !(dim == Dim::of(0, 0, -1)) && !(dim == Dim::length())) {
        *error = tr("%1: the frequency is in %2: give Hz, or a wavelength in m").arg(node.name(), dimName(dim));
        return false;
    }
    const int count = std::clamp(node.integer(QStringLiteral("modes")), 1, 200);
    // The domains whose modes it keeps (none: those nearest the index).
    std::vector<char> track;
    {
        QString trackWhy;
        const QVector<int> picked = resolveSelection(s.topology(), s.model(), Level::Domain, node.value(QStringLiteral("track")).toObject(), &trackWhy);
        if (!trackWhy.isEmpty()) {
            *error = tr("%1: the domains it tracks: %2").arg(node.name(), trackWhy);
            return false;
        }
        if (!picked.isEmpty()) {
            track.assign(s.topology().domains.size(), 0);
            for (int d : picked) track[std::size_t(d)] = 1;
        }
    }
    double guess = 0;
    if (!node.text(QStringLiteral("neff")).trimmed().isEmpty()) {
        const std::optional<double> g = evaluateConstant(node.text(QStringLiteral("neff")), s.parameters().scope, Dim(), &why);
        if (!g || !(*g > 0)) {
            *error = tr("%1: the effective index searched about: %2").arg(node.name(), g ? tr("it must be more than 0") : why);
            return false;
        }
        guess = *g;
    }
    std::vector<int> modal, quasi;
    for (int i = 0; i < int(st.problems.size()); ++i) (st.problems[std::size_t(i)]->modal() ? modal : quasi).push_back(i);
    if (modal.empty()) {
        *error = tr("%1: none of its physics has modes (Electromagnetic Waves, Mode Analysis has)").arg(node.name());
        return false;
    }
    if (modal.size() > 1) *warnings << tr("%1: the modes of its first mode analysis only").arg(node.name());
    for (auto& p : st.problems)
        if (!p->field().solved) p->initialize();
    s.frequencyDomain = true;
    s.modeAnalysis = true;
    detail::Problem& modes = *st.problems[std::size_t(modal.front())];
    const QString tag = modes.field().tag;
    // (One mode asked for: it is the branch followed. Several are the set
    // nearest the index - a dispersion's modes, kept as they are.)
    const bool follow = node.flag(QStringLiteral("follow")) && count == 1;
    if (follow && s.modeCentres.value(tag) > 0) guess = s.modeCentres.value(tag);
    for (std::size_t k = 0; k < frequencies.size(); ++k) {
        const double f = frequencies[k];
        if (!tell(double(k) / double(frequencies.size()), tr("%1: modes at %2").arg(node.name(), formatQuantity(f, Dim::of(0, 0, -1))))) {
            *error = tr("cancelled");
            return false;
        }
        s.setFrequency(f);
        if (!quasi.empty() && !solveTogether(st, quasi, [&](int) { return st.options; }, false, error)) return false;
        if (!solvedBefore(st, quasi, error)) return false;
        SolveOptions o = st.options;
        o.harmonic = true;
        o.frequency = f;
        o.track = track;
        o.polarization = node.text(QStringLiteral("polarization")) == QLatin1String("te")   ? 1
                         : node.text(QStringLiteral("polarization")) == QLatin1String("tm") ? 2
                                                                                             : 0;
        const int found = modes.solveModes(o, count, guess, error);
        if (found < 0) return false;
        if (found == 0) *warnings << tr("%1: no mode found at %2").arg(node.name(), formatQuantity(f, Dim::of(0, 0, -1)));
        double centre = 0;
        for (int m = 0; m < found; ++m) {
            modes.selectMode(m);
            for (auto& p : st.problems) p->setGlobals();
            s.addSnapshot();
            centre += s.global(tag + QStringLiteral(".neff")).value_or(0) / found;
        }
        // Where the next frequency (a sweep's next point) searches.
        if (found > 0 && centre > 0) {
            s.modeCentres.insert(tag, centre);
            if (follow) guess = centre;
        }
        // Left selected for the steps after: the mode a semiconductor's
        // Optical Generation asks for, else the first (each mode in turn
        // left the last, the lowest index, lighting a photodiode).
        if (found > 0) {
            const int lit = std::clamp(litMode(s, modes.field().tag), 1, found);
            if (lit < found) {
                modes.selectMode(lit - 1);
                for (auto& p : st.problems) p->setGlobals();
            }
        }
    }
    return true;
}

bool eigenvalueStep(Solution& s, const Node& node, const std::function<bool(double, const QString&)>& told, QStringList* warnings, QString* error)
{
    Telling tell{told, 0, node.name()};
    Step st{s, node, {}, {}, {}, 50, 1e-6};
    if (!setupStep(st, error)) return false;
    cancelWith(st, tell);
    std::vector<int> pdes, others;
    for (int i = 0; i < int(st.problems.size()); ++i)
        (st.problems[std::size_t(i)]->field().type == QLatin1String("pde") ? pdes : others).push_back(i);
    if (pdes.empty()) {
        *error = tr("%1: none of its physics has eigenvalues (a PDE has)").arg(node.name());
        return false;
    }
    if (pdes.size() > 1) *warnings << tr("%1: its first PDE's eigenvalues only").arg(node.name());
    QString why;
    const std::optional<double> shift = evaluateConstant(node.text(QStringLiteral("shift")), s.parameters().scope, Dim(), &why);
    if (!shift) {
        *error = tr("%1: what it searches about: %2").arg(node.name(), why);
        return false;
    }
    for (auto& p : st.problems)
        if (!p->field().solved) p->initialize();
    if (!tell(0.1, tr("%1: the eigenvalues").arg(node.name()))) {
        *error = tr("cancelled");
        return false;
    }
    if (!others.empty() && !solveTogether(st, others, [&](int) { return st.options; }, false, error)) return false;
    if (!solvedBefore(st, others, error)) return false;
    s.frequencyDomain = true;
    s.modeAnalysis = true;
    detail::Problem& e = *st.problems[std::size_t(pdes.front())];
    const int found = e.solveModes(st.options, std::clamp(node.integer(QStringLiteral("count")), 1, 200), *shift, error);
    if (found < 0) return false;
    if (found == 0) *warnings << tr("%1: no eigenvalue found").arg(node.name());
    for (int k = 0; k < found; ++k) {
        e.selectMode(k);
        for (auto& p : st.problems) p->setGlobals();
        s.addSnapshot();
    }
    return true;
}

/// A step's quantum wells physics (by problem) and the others, solved first.
bool splitQuantum(const Step& st, std::vector<int>& quantum, std::vector<int>& others, QStringList* warnings, QString* error)
{
    for (int i = 0; i < int(st.problems.size()); ++i)
        (st.problems[std::size_t(i)]->field().type == QLatin1String("quantum") ? quantum : others).push_back(i);
    if (quantum.empty()) {
        *error = tr("%1: none of its physics has quantum states (Quantum Wells has)").arg(st.node.name());
        return false;
    }
    if (quantum.size() > 1) *warnings << tr("%1: its first quantum wells physics only").arg(st.node.name());
    return true;
}

bool statesStep(Solution& s, const Node& node, const std::function<bool(double, const QString&)>& told, QStringList* warnings, QString* error)
{
    Telling tell{told, 0, node.name()};
    Step st{s, node, {}, {}, {}, 50, 1e-6};
    if (!setupStep(st, error)) return false;
    cancelWith(st, tell);
    std::vector<int> quantum, others;
    if (!splitQuantum(st, quantum, others, warnings, error)) return false;
    for (auto& p : st.problems)
        if (!p->field().solved) p->initialize();
    if (!tell(0.1, tr("%1: the states").arg(node.name()))) {
        *error = tr("cancelled");
        return false;
    }
    // What the states read (a semiconductor's potential) first.
    if (!others.empty() && !solveTogether(st, others, [&](int) { return st.options; }, false, error)) return false;
    if (!solvedBefore(st, others, error)) return false;
    detail::Problem& q = *st.problems[std::size_t(quantum.front())];
    const int count = std::clamp(node.integer(QStringLiteral("states")), 1, 100);
    // Self-consistent: the states, then the semiconductor's carriers moved
    // to theirs and it solved again, until those rest.
    if (node.flag(QStringLiteral("consistent"))) {
        const int most = std::max(1, node.integer(QStringLiteral("iterations")));
        QString why;
        const double tolerance = evaluateConstant(node.text(QStringLiteral("sptolerance")), s.parameters().scope, Dim(), &why).value_or(1e-4);
        double change = NaN;
        int turn = 0;
        for (turn = 1; turn <= most; ++turn) {
            if (q.solveModes(st.options, count, 0, error) < 0) return false;
            q.selectMode(0);
            change = 0;
            for (int i : others) {
                const double c = st.problems[std::size_t(i)]->quantumCorrection(error);
                if (std::isnan(c)) return false;
                change = std::max(change, c);
            }
            if (!tell(0.1 + 0.8 * turn / most, tr("%1: Schrödinger-Poisson, turn %2 (%3 Vt)").arg(node.name()).arg(turn).arg(change, 0, 'g', 3))) {
                *error = tr("cancelled");
                return false;
            }
            if (change <= tolerance) break;
            if (!solveTogether(st, others, [&](int) { return st.options; }, false, error)) return false;
        }
        if (change <= tolerance)
            s.log << tr("%1: Schrödinger-Poisson in %2 turns (the quantum potential's last change %3 Vt)").arg(node.name()).arg(turn).arg(change, 0, 'g', 3);
        else
            *warnings << tr("%1: Schrödinger-Poisson not at rest in %2 turns: the quantum potential still moving %3 Vt").arg(node.name()).arg(most).arg(change, 0, 'g', 3);
    }
    s.frequencyDomain = true;
    s.modeAnalysis = true;
    s.quantumStates = true;
    const int found = q.solveModes(st.options, count, 0, error);
    if (found < 0) return false;
    for (int k = 0; k < found; ++k) {
        q.selectMode(k);
        for (auto& p : st.problems) p->setGlobals();
        s.addSnapshot();
    }
    return true;
}

bool spectrumStep(Solution& s, const Node& node, const std::function<bool(double, const QString&)>& told, QStringList* warnings, QString* error)
{
    Telling tell{told, 0, node.name()};
    Step st{s, node, {}, {}, {}, 50, 1e-6};
    if (!setupStep(st, error)) return false;
    cancelWith(st, tell);
    std::vector<int> quantum, others;
    if (!splitQuantum(st, quantum, others, warnings, error)) return false;
    constexpr double q = 1.602176634e-19, h = 6.62607015e-34, c0 = 299792458.0;
    const Scope& scope = s.parameters().scope;
    // Photon energies (eV, J), free-space wavelengths or frequencies.
    Dim dim;
    QString why;
    const std::optional<std::vector<double>> listed = valueList(node.text(QStringLiteral("energies")), scope, &dim, &why);
    if (!listed) {
        *error = tr("%1: the photon energies: %2").arg(node.name(), why);
        return false;
    }
    detail::SpectrumOptions o;
    o.solve = st.options;
    int axis = 1;
    if (dim == Dim::length()) axis = 2;
    else if (dim == Dim::of(0, 0, -1)) axis = 0;
    else if (!dim.isNone() && !(dim == Dim::of(2, 1, -2))) {
        *error = tr("%1: the photon energies are in %2: give eV, a wavelength in m or a frequency in Hz").arg(node.name(), dimName(dim));
        return false;
    }
    for (double v : *listed) {
        const double E = axis == 2 ? (v > 0 ? h * c0 / v : 0) : axis == 0 ? h * v : dim.isNone() ? v * q : v;
        if (!(E > 0) || !std::isfinite(E)) {
            *error = tr("%1: a photon energy of %2: they must be more than 0").arg(node.name(), formatNumber(v));
            return false;
        }
        o.energies.push_back(E);
    }
    if (o.energies.empty()) {
        *error = tr("%1: give a photon energy at least").arg(node.name());
        return false;
    }
    // An energy: eV as a plain number or a voltage, J as such.
    auto energy = [&](const QString& key, const QString& what, double* out) {
        const Expression e = Expression::compile(node.text(key), scope);
        if (!e.isValid() || !e.isConstant() || e.constant() < 0) {
            *error = tr("%1: %2: %3").arg(node.name(), what, e.isValid() ? tr("it must be a constant of 0 or more") : e.error());
            return false;
        }
        *out = e.dim() == Dim::of(2, 1, -2) ? e.constant() : e.constant() * q;
        return true;
    };
    if (!energy(QStringLiteral("linewidth"), tr("its broadening"), &o.linewidth)) return false;
    const QString shape = node.text(QStringLiteral("lineshape"));
    o.lineshape = shape == QLatin1String("gaussian") ? 1 : shape == QLatin1String("sech") ? 2 : 0;
    o.excitons = node.flag(QStringLiteral("excitons"));
    o.excitonModel = node.text(QStringLiteral("excitonmodel")) == QLatin1String("bastard") ? 1 : 0;
    o.continuum = node.text(QStringLiteral("continuum")) == QLatin1String("bound") ? 1 : 0;
    if (!energy(QStringLiteral("fade"), tr("its fade"), &o.fade)) return false;
    if (!node.text(QStringLiteral("dlinewidth")).trimmed().isEmpty()) {
        // (Energy per field: C m.)
        const std::optional<double> d = evaluateConstant(node.text(QStringLiteral("dlinewidth")), scope, Dim::of(1, 0, 1, 1), &why);
        if (!d || *d < 0) {
            *error = tr("%1: the broadening's growth with the field: %2").arg(node.name(), d ? tr("it must be 0 or more") : why);
            return false;
        }
        o.dlinewidth = *d;
    }
    if (!node.text(QStringLiteral("epsr")).trimmed().isEmpty()) {
        const std::optional<double> e = evaluateConstant(node.text(QStringLiteral("epsr")), scope, Dim(), &why);
        if (!e || !(*e > 0)) {
            *error = tr("%1: the excitons' permittivity: %2").arg(node.name(), e ? tr("it must be more than 0") : why);
            return false;
        }
        o.epsr = *e;
    }
    const std::optional<double> density = evaluateConstant(node.text(QStringLiteral("density")), scope, Dim::of(-3), &why);
    if (!density || *density < 0) {
        *error = tr("%1: the carriers: %2").arg(node.name(), density ? tr("they must be 0 or more") : why);
        return false;
    }
    o.density = *density;
    if (!node.text(QStringLiteral("nr")).trimmed().isEmpty()) {
        const std::optional<double> n = evaluateConstant(node.text(QStringLiteral("nr")), scope, Dim(), &why);
        if (!n || !(*n > 0)) {
            *error = tr("%1: the refractive index: %2").arg(node.name(), n ? tr("it must be more than 0") : why);
            return false;
        }
        o.index = *n;
    }
    o.states = std::clamp(node.integer(QStringLiteral("states")), 1, 50);
    // (The k·p bands' in-plane k grid.)
    o.kpoints = node.text(QStringLiteral("kpoints")).trimmed().isEmpty() ? 121 : std::clamp(node.integer(QStringLiteral("kpoints")), 11, 2000);
    for (auto& p : st.problems)
        if (!p->field().solved) p->initialize();
    if (!tell(0.1, tr("%1: the spectrum").arg(node.name()))) {
        *error = tr("cancelled");
        return false;
    }
    if (!others.empty() && !solveTogether(st, others, [&](int) { return st.options; }, false, error)) return false;
    if (!solvedBefore(st, others, error)) return false;
    s.frequencyDomain = true;
    s.photonAxis = axis;
    detail::Problem& qp = *st.problems[std::size_t(quantum.front())];
    const int points = qp.solveSpectrum(o, error);
    if (points < 0) return false;
    s.setMode(0);
    for (int k = 0; k < points; ++k) {
        s.setFrequency(o.energies[std::size_t(k)] / h);
        qp.selectSpectrum(k);
        for (auto& p : st.problems) p->setGlobals();
        s.addSnapshot();
    }
    return true;
}

bool bandsStep(Solution& s, const Node& node, const std::function<bool(double, const QString&)>& told, QStringList* warnings, QString* error)
{
    Telling tell{told, 0, node.name()};
    Step st{s, node, {}, {}, {}, 50, 1e-6};
    if (!setupStep(st, error)) return false;
    cancelWith(st, tell);
    std::vector<int> quantum, others;
    if (!splitQuantum(st, quantum, others, warnings, error)) return false;
    const Scope& scope = s.parameters().scope;
    QString why;
    detail::SpectrumOptions o;
    o.solve = st.options;
    const std::optional<double> kmax = evaluateConstant(node.text(QStringLiteral("kmax")), scope, Dim::of(-1), &why);
    if (!kmax || !(*kmax > 0)) {
        *error = tr("%1: the largest k: %2").arg(node.name(), kmax ? tr("it must be more than 0") : why);
        return false;
    }
    o.kmax = *kmax;
    o.kpoints = std::clamp(node.integer(QStringLiteral("points")), 2, 2000);
    o.states = std::clamp(node.integer(QStringLiteral("states")), 1, 50);
    for (auto& p : st.problems)
        if (!p->field().solved) p->initialize();
    if (!tell(0.1, tr("%1: the valence bands").arg(node.name()))) {
        *error = tr("cancelled");
        return false;
    }
    if (!others.empty() && !solveTogether(st, others, [&](int) { return st.options; }, false, error)) return false;
    if (!solvedBefore(st, others, error)) return false;
    s.frequencyDomain = true;
    s.photonAxis = 3;
    detail::Problem& qp = *st.problems[std::size_t(quantum.front())];
    const int points = qp.solveBands(o, error);
    if (points < 0) return false;
    s.setMode(0);
    for (int k = 0; k < points; ++k) {
        s.setFrequency(o.kpoints > 1 ? o.kmax * k / (o.kpoints - 1) : 0.0);
        qp.selectBands(k);
        for (auto& p : st.problems) p->setGlobals();
        s.addSnapshot();
    }
    return true;
}

bool frequencyStep(Solution& s, const Node& node, const std::function<bool(double, const QString&)>& told, QStringList* warnings, QString* error)
{
    Telling tell{told, 0, node.name()};
    Step st{s, node, {}, {}, {}, 50, 1e-6};
    if (!setupStep(st, error)) return false;
    cancelWith(st, tell);
    Dim dim;
    QString why;
    const std::optional<std::vector<double>> listed = valueList(node.text(QStringLiteral("frequencies")), s.parameters().scope, &dim, &why);
    if (!listed) {
        *error = tr("%1: the frequencies: %2").arg(node.name(), why);
        return false;
    }
    if (!dim.isNone() && !(dim == Dim::of(0, 0, -1))) {
        *error = tr("%1: the frequencies are in %2, not in Hz").arg(node.name(), dimName(dim));
        return false;
    }
    std::vector<double> frequencies = *listed;
    for (double f : frequencies)
        if (!(f >= 0) || !std::isfinite(f)) {
            *error = tr("%1: a frequency of %2: they must be 0 or more").arg(node.name(), formatNumber(f));
            return false;
        }
    if (frequencies.empty()) {
        *error = tr("%1: give a frequency at least").arg(node.name());
        return false;
    }
    bool any = false;
    for (const auto& p : st.problems) any = any || p->harmonic();
    if (!any)
        *warnings << tr("%1: none of its physics has a frequency domain (magnetic fields have): each frequency is solved at rest")
                         .arg(node.name());
    for (auto& p : st.problems)
        if (!p->field().solved) p->initialize();
    // After a mode analysis whose light this step reads (a photodiode's,
    // modulated): its frequencies a series of their own, each with the mode's
    // fields and globals as they are - not after the mode's snapshot at the
    // light's own frequency.
    if (s.modeAnalysis) {
        s.clearSnapshots();
        s.modeAnalysis = s.quantumStates = false;
        s.photonAxis = 0;
        s.setMode(0);
    }
    s.frequencyDomain = true;
    const bool sweep = node.flag(QStringLiteral("sweep"));
    std::vector<int> all;
    for (int i = 0; i < int(st.problems.size()); ++i) all.push_back(i);
    for (std::size_t k = 0; k < frequencies.size(); ++k) {
        const double f = frequencies[k];
        if (!tell(double(k) / double(frequencies.size()), tr("%1: f = %2").arg(node.name(), formatQuantity(f, Dim::of(0, 0, -1))))) {
            *error = tr("cancelled");
            return false;
        }
        s.setFrequency(f);
        auto optionsOf = [&](int i) {
            SolveOptions o = st.options;
            if (st.problems[std::size_t(i)]->harmonic()) {
                o.harmonic = true;
                o.frequency = f;
                o.sweep = sweep;
            }
            return o;
        };
        QString failed;
        if (!solveTogether(st, all, optionsOf, false, &failed)) {
            if (failed.contains(QLatin1String("did not converge"))) *warnings << failed;
            else {
                *error = failed;
                return false;
            }
        }
        for (auto& p : st.problems) p->setGlobals();
        s.addSnapshot();
    }
    s.log << tr("%1: %2 frequencies, from %3 to %4")
                 .arg(node.name())
                 .arg(frequencies.size())
                 .arg(formatQuantity(frequencies.front(), Dim::of(0, 0, -1)), formatQuantity(frequencies.back(), Dim::of(0, 0, -1)));
    return true;
}

bool transientStep(Solution& s, const Node& node, const std::function<bool(double, const QString&)>& told, QStringList* warnings, QString* error)
{
    Telling tell{told, 0, node.name()};
    Step st{s, node, {}, {}, {}, 50, 1e-6};
    if (!setupStep(st, error)) return false;
    cancelWith(st, tell);
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
        *warnings << tr("%1: none of its physics changes in time (heat, a semiconductor's carriers and a PDE with a mass do): each "
                        "output time is solved at rest")
                         .arg(node.name());
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
    // After a mode analysis whose light this step reads (a photodiode's
    // pulse response): its times a series of their own, the mode's fields
    // and globals as they are.
    if (s.modeAnalysis) {
        s.clearSnapshots();
        s.modeAnalysis = s.quantumStates = s.frequencyDomain = false;
        s.photonAxis = 0;
        s.setMode(0);
    }
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
                       const QString& studyTag, const Progress& progress, const Solution* start)
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
    if (start) solution->modeCentres = start->modeCentres;
    if (start && start->meshPtr() == mesh)
        for (Field& f : solution->fields()) {
            if (f.type != QLatin1String("semiconductor")) continue;
            const int from = start->fieldIndex(f.tag);
            if (from < 0) continue;
            const Field& g = start->fields()[std::size_t(from)];
            // (The same space: a semiconductor's values, solved, are seven
            // arrays by node - V, n, p, the doping, the bands' - a field
            // made fresh has only its unknowns' room, and compared so none
            // was taken: each point solved from equilibrium again.)
            if (!g.solved || g.type != f.type || !g.space || !f.space || g.space->size() != f.space->size()) continue;
            f.values = g.values;
            f.terminals = g.terminals;
            f.solved = true;
        }
    if (solution->axisymmetric()) {
        const QRectF b = mesh->bounds();
        if (b.left() < -1e-9 * std::max(b.width(), b.height())) {
            out.error = tr("an axisymmetric model lies where r ≥ 0: its geometry reaches x = %1").arg(formatNumber(b.left(), 6));
            return out;
        }
    }
    std::vector<const Node*> steps;
    bool inTime = false, inFrequency = false;
    for (const Node& step : study->children)
        if (step.enabled && step.type != QLatin1String("sweep")) {
            steps.push_back(&step);
            // (A mode analysis before a step in time is its light: a
            // photodiode's pulse response. After one, it is a frequency's.)
            inFrequency = inFrequency || step.type == QLatin1String("frequency") || (step.type == QLatin1String("mode") && inTime)
                          || step.type == QLatin1String("states") || step.type == QLatin1String("spectrum") || step.type == QLatin1String("bands")
                          || step.type == QLatin1String("eigenvalue");
            inTime = inTime || step.type == QLatin1String("transient");
        }
    if (steps.empty()) {
        out.error = tr("%1 has no step to solve").arg(study->name());
        return out;
    }
    if (inTime && inFrequency) {
        out.error = tr("%1 steps in time and in frequency: a study has one or the other").arg(study->name());
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
        else if (step.type == QLatin1String("frequency")) ok = frequencyStep(*solution, step, tell, &out.warnings, &why);
        else if (step.type == QLatin1String("mode")) ok = modeStep(*solution, step, tell, &out.warnings, &why);
        else if (step.type == QLatin1String("states")) ok = statesStep(*solution, step, tell, &out.warnings, &why);
        else if (step.type == QLatin1String("eigenvalue")) ok = eigenvalueStep(*solution, step, tell, &out.warnings, &why);
        else if (step.type == QLatin1String("spectrum")) ok = spectrumStep(*solution, step, tell, &out.warnings, &why);
        else if (step.type == QLatin1String("bands")) ok = bandsStep(*solution, step, tell, &out.warnings, &why);
        else why = tr("%1: a %2 step is not solved yet").arg(study->name(), step.name());
        if (!ok) {
            out.error = why;
            return out;
        }
    }
    // What the physics said in the log as warnings (a Newton's method that
    // did not converge): the study's warnings too.
    for (const QString& line : solution->log)
        if (line.startsWith(QLatin1String("Warning: ")) && !out.warnings.contains(line.mid(9))) out.warnings << line.mid(9);
    solution->seconds = timer.elapsed() / 1000.0;
    int dofs = 0;
    for (const Field& f : solution->fields())
        if (f.solved) dofs += f.space->size() * f.count();
    solution->log << tr("Solved %1 degrees of freedom in %2 s").arg(dofs).arg(solution->seconds, 0, 'f', 2);
    if (progress) progress(1.0, tr("Done"));
    out.solution = solution;
    return out;
}

std::vector<double> errorIndicators(const Solution& s, int fi)
{
    const Field& f = s.fields()[std::size_t(fi)];
    if (f.edges || !f.space) return {};
    const Space& sp = *f.space;
    const Mesh& mesh = s.mesh();
    const int nt = sp.elements();
    const int nn = int(mesh.nodes.size());
    const int comps = std::min(2, f.count());
    const std::size_t n = std::size_t(sp.size());
    // The gradient recovered at each corner node, by domain: the triangles'
    // gradients there weighed by their areas.
    std::map<std::pair<int, int>, std::array<double, 5>> recovered;   // (node, domain): Σ a gx, gy (each component), Σ a
    auto gradientAt = [&](int t, double xi, double eta, int c, double& gx, double& gy) {
        Shape sh;
        shapeAt(sp.order(), xi, eta, sh);
        const Space::Map m = sp.map(t, xi, eta);
        double rx = 0, ry = 0;
        for (int i = 0; i < sh.count; ++i) {
            const double v = f.values[std::size_t(c) * n + std::size_t(sp.dofs(t)[i])];
            rx += sh.dxi[i] * v;
            ry += sh.deta[i] * v;
        }
        Space::gradient(m, rx, ry, gx, gy);
    };
    static const double corner[3][2] = {{0, 0}, {1, 0}, {0, 1}};
    for (int t = 0; t < nt; ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        const double area = std::abs(sp.map(t, 1.0 / 3, 1.0 / 3).det) / 2;
        for (int k = 0; k < 3; ++k) {
            std::array<double, 5>& r = recovered[{mesh.triangles[std::size_t(t)][std::size_t(k)], d}];
            for (int c = 0; c < comps; ++c) {
                double gx = 0, gy = 0;
                gradientAt(t, corner[k][0], corner[k][1], c, gx, gy);
                r[std::size_t(2 * c)] += area * gx;
                r[std::size_t(2 * c + 1)] += area * gy;
            }
            r[4] += area;
        }
    }
    (void)nn;
    std::vector<double> eta(std::size_t(nt), 0.0);
    const TriangleRule& rule = triangleRule(4);
    for (int t = 0; t < nt; ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        double e2 = 0;
        const auto& tri = mesh.triangles[std::size_t(t)];
        for (const auto& q : rule.points) {
            const double lambda[3] = {1 - q[0] - q[1], q[0], q[1]};
            const double w = q[2] * std::abs(sp.map(t, q[0], q[1]).det);
            for (int c = 0; c < comps; ++c) {
                double gx = 0, gy = 0;
                gradientAt(t, q[0], q[1], c, gx, gy);
                double Gx = 0, Gy = 0;
                for (int k = 0; k < 3; ++k) {
                    const std::array<double, 5>& r = recovered[{tri[std::size_t(k)], d}];
                    Gx += lambda[k] * r[std::size_t(2 * c)] / r[4];
                    Gy += lambda[k] * r[std::size_t(2 * c + 1)] / r[4];
                }
                e2 += w * ((gx - Gx) * (gx - Gx) + (gy - Gy) * (gy - Gy));
            }
        }
        eta[std::size_t(t)] = std::sqrt(e2);
    }
    return eta;
}

namespace {

/// A study solved, its mesh made finer where the error indicator of its
/// adaptive step's physics is largest (the triangles that hold half of the
/// squared error, at half their size), and solved again, as often as that
/// step asks. (In frequency: the last frequency's error.)
StudyResult solveAdaptive(const Model& model, std::shared_ptr<ParameterScope> parameters, std::shared_ptr<const Mesh> mesh,
                          const QString& studyTag, const Progress& progress, const Solution* start = nullptr)
{
    const Node* study = nullptr;
    for (const Node* s : model.studies())
        if (studyTag.isEmpty() ? !study : s->tag == studyTag) study = s;
    const Node* adaptive = nullptr;
    if (study)
        for (const Node& step : study->children)
            if (step.enabled && (step.type == QLatin1String("stationary") || step.type == QLatin1String("frequency"))
                && step.flag(QStringLiteral("adaptive")))
                adaptive = &step;
    const int rounds = adaptive ? std::clamp(adaptive->integer(QStringLiteral("adaptsteps")), 1, 20) : 0;
    auto part = [&](int r) {
        return Progress([&, r](double f, const QString& w) {
            return !progress || progress((r + f) / (rounds + 1), rounds ? tr("mesh %1 of %2: %3").arg(r + 1).arg(rounds + 1).arg(w) : w);
        });
    };
    StudyResult r = solveStudy(model, parameters, mesh, studyTag, part(0), start);
    if (!adaptive || !r.solution) return r;
    QString why;
    MeshSizes sizes = meshSizes(model, *parameters, *mesh->topology, &why);
    QStringList said;
    for (int round = 1; round <= rounds; ++round) {
        Solution& s = *r.solution;
        // Its physics: the one named, else the first solved.
        int fi = -1;
        const QStringList names = adaptive->list(QStringLiteral("adaptphysics"));
        for (int i = 0; i < int(s.fields().size()); ++i)
            if (s.fields()[std::size_t(i)].solved && (names.isEmpty() ? fi < 0 : names.contains(s.fields()[std::size_t(i)].tag))) fi = i;
        if (fi < 0) {
            r.warnings << tr("%1: no physics to refine the mesh by").arg(adaptive->name());
            break;
        }
        const std::vector<double> eta = errorIndicators(s, fi);
        if (eta.empty()) {
            r.warnings << tr("%1: %2's error is not estimated (edge elements): the mesh is not refined").arg(adaptive->name(), s.fields()[std::size_t(fi)].tag);
            break;
        }
        const Mesh& m = s.mesh();
        double total = 0;
        std::vector<int> order;
        for (int t = 0; t < int(eta.size()); ++t) {
            total += eta[std::size_t(t)] * eta[std::size_t(t)];
            if (eta[std::size_t(t)] > 0) order.push_back(t);
        }
        said << tr("%1 triangles, error %2").arg(m.triangles.size()).arg(std::sqrt(total), 0, 'g', 3);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return eta[std::size_t(a)] > eta[std::size_t(b)]; });
        double marked = 0;
        for (int t : order) {
            if (marked >= 0.5 * total) break;
            marked += eta[std::size_t(t)] * eta[std::size_t(t)];
            const auto& tri = m.triangles[std::size_t(t)];
            const QPointF a = m.nodes[std::size_t(tri[0])], b = m.nodes[std::size_t(tri[1])], c = m.nodes[std::size_t(tri[2])];
            const double area = std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2;
            const double h = std::sqrt(4 * area / std::sqrt(3.0));
            const QPointF centre = (a + b + c) / 3.0;
            sizes.points.push_back({centre.x(), centre.y(), h / 2});
        }
        const MeshBuild finer = buildMesh(m.topology, sizes, part(round));
        if (!finer.mesh) {
            r.warnings << tr("%1: the finer mesh: %2").arg(adaptive->name(), finer.error);
            break;
        }
        StudyResult next = solveStudy(model, parameters, finer.mesh, studyTag, part(round));
        if (!next.solution) {
            r.warnings << tr("%1: on the finer mesh: %2").arg(adaptive->name(), next.error);
            break;
        }
        r = next;
    }
    said << tr("%1 triangles at last").arg(r.solution->mesh().triangles.size());
    r.solution->log << tr("Adaptive mesh refinement: %1").arg(said.join(QLatin1String("; ")));
    return r;
}

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
        const StudyResult r = solveAdaptive(model, parameters, run.mesh, study->tag,
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
    std::shared_ptr<const Solution> previous;
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
        const StudyResult r = solveAdaptive(model, ps, m, study->tag, sub(0.3, 1), previous.get());
        run.warnings << r.warnings;
        if (!r.solution) {
            run.error = tr("%1: %2").arg(where, r.error);
            return run;
        }
        previous = r.solution;
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
