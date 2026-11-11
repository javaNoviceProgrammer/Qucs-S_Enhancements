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
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>

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
                                                 {"currents", "V", Dim::voltage(), 0},
                                                 {"solid", "u", Dim::length(), 0},
                                                 {"magnetic", "A", Dim::of(1, 1, -2, -1), 0},
                                                 {"semiconductor", "V", Dim::voltage(), 0}};
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
        if (field.type == QLatin1String("solid")) {
            defineSolid(f);
            continue;
        }
        if (field.type == QLatin1String("magnetic")) {
            defineMagnetic(f);
            continue;
        }
        if (field.type == QLatin1String("semiconductor")) {
            defineSemiconductor(f);
            continue;
        }
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
                // (A piezoelectric solid's polarization P = e S in D too.)
                const QString pkey = QStringLiteral("P") + QString::fromLatin1(axis[i]);
                flux[i] = define(name, es ? dfield : jfield, what, K::Derived, f, 0, 1, [entry, e, i, scale, es, f, pkey](PointEvaluator3& ev) {
                    double s = 0;
                    for (int j = 0; j < 3; ++j) s += entry(ev, 3 * i + j) * ev.value(e[j]);
                    double p = 0;
                    if (es) {
                        const auto& named = ev.solution().coefficients()[std::size_t(f)].named;
                        const auto it = named.find(pkey);
                        const int d = ev.domain();
                        if (it != named.end() && d >= 0 && d < int(it->second.size()) && it->second[std::size_t(d)].isValid()) {
                            p = ev.eval(it->second[std::size_t(d)]);
                            if (!std::isfinite(p)) p = 0;
                        }
                    }
                    return scale * s + p;
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

int Solution3::definePart(const QString& name, const Dim& dim, const QString& description, int field, int part, int axis)
{
    const bool known = a_pointScope->variable(name) >= 0;
    const int slot = define(name, dim, description, axis < 0 ? VariableDef::Value : VariableDef::Gradient, field, std::max(axis, 0));
    if (slot >= 0 && !known) a_defs[std::size_t(slot)].part = part;
    return slot;
}

void Solution3::defineSolid(int f)
{
    using K = VariableDef::Kind;
    const Field3& field = a_fields[std::size_t(f)];
    const QString p = field.tag + QLatin1Char('.');
    const Dim pressure = Dim::of(-1, 1, -2);
    const char* axis[3] = {"x", "y", "z"};
    int u[3];
    for (int c = 0; c < 3; ++c) {
        const QString name = field.components.value(c);
        u[c] = definePart(p + name, Dim::length(), tr("displacement, %1 component").arg(QString::fromLatin1(axis[c])), f, c);
        definePart(name, Dim::length(), tr("displacement, %1 component (%2)").arg(QString::fromLatin1(axis[c]), field.tag), f, c);
    }
    define(p + QStringLiteral("disp"), Dim::length(), tr("displacement, magnitude"), K::Derived, f, 0, 1, [u](PointEvaluator3& e) {
        const double a = e.value(u[0]), b = e.value(u[1]), c = e.value(u[2]);
        return std::sqrt(a * a + b * b + c * c);
    });
    // (Its strain and stress at the point, Voigt's order xx, yy, zz, yz, xz,
    // xy: a tensor's shear strain half the engineering one.)
    struct Out {
        const char* name;
        bool stress;
        int k;
        double factor;
        const char* what;
    };
    static const Out outs[] = {
        {"ex", false, 0, 1, "strain, x component"},
        {"ey", false, 1, 1, "strain, y component"},
        {"ez", false, 2, 1, "strain, z component"},
        {"eyz", false, 3, 0.5, "shear strain, yz (tensor: half the engineering shear)"},
        {"exz", false, 4, 0.5, "shear strain, xz (tensor: half the engineering shear)"},
        {"exy", false, 5, 0.5, "shear strain, xy (tensor: half the engineering shear)"},
        {"sx", true, 0, 1, "stress, x component"},
        {"sy", true, 1, 1, "stress, y component"},
        {"sz", true, 2, 1, "stress, z component"},
        {"syz", true, 3, 1, "shear stress, yz"},
        {"sxz", true, 4, 1, "shear stress, xz"},
        {"sxy", true, 5, 1, "shear stress, xy"},
    };
    for (const Out& o : outs) {
        const bool stress = o.stress;
        const int k = o.k;
        const double factor = o.factor;
        define(p + QString::fromLatin1(o.name), stress ? pressure : Dim(), tr(o.what), K::Derived, f, 0, 1,
               [f, stress, k, factor](PointEvaluator3& e) {
                   detail3::SolidState st;
                   if (!detail3::solidStateAt(e, f, st)) return NaN;
                   return factor * (stress ? st.stress[k] : st.strain[k]);
               });
    }
    define(p + QStringLiteral("Ws"), pressure, tr("strain energy density"), K::Derived, f, 0, 1, [f](PointEvaluator3& e) {
        detail3::SolidState st;
        return detail3::solidStateAt(e, f, st) ? st.energy : NaN;
    });
    define(p + QStringLiteral("mises"), pressure, tr("von Mises stress"), K::Derived, f, 0, 1, [f](PointEvaluator3& e) {
        detail3::SolidState st;
        if (!detail3::solidStateAt(e, f, st)) return NaN;
        const double* s = st.stress;
        return std::sqrt(0.5 * ((s[0] - s[1]) * (s[0] - s[1]) + (s[1] - s[2]) * (s[1] - s[2]) + (s[2] - s[0]) * (s[2] - s[0]))
                         + 3 * (s[3] * s[3] + s[4] * s[4] + s[5] * s[5]));
    });
    define(p + QStringLiteral("p"), pressure, tr("pressure: minus the mean of the normal stresses"), K::Derived, f, 0, 1, [f](PointEvaluator3& e) {
        detail3::SolidState st;
        return detail3::solidStateAt(e, f, st) ? -(st.stress[0] + st.stress[1] + st.stress[2]) / 3 : NaN;
    });
    // Its material at the point: E and ν (isotropic), α and T - Tref.
    auto named = [f](PointEvaluator3& e, const QString& key) {
        const auto& m = e.solution().coefficients()[std::size_t(f)].named;
        const auto it = m.find(key);
        const int d = e.domain();
        if (it == m.end() || d < 0 || d >= int(it->second.size()) || !it->second[std::size_t(d)].isValid()) return NaN;
        return e.eval(it->second[std::size_t(d)]);
    };
    define(p + QStringLiteral("E"), pressure, tr("Young's modulus (an isotropic solid's)"), K::Derived, f, 0, 1,
           [named](PointEvaluator3& e) { return named(e, QStringLiteral("E")); });
    define(p + QStringLiteral("nu"), Dim(), tr("Poisson's ratio (an isotropic solid's)"), K::Derived, f, 0, 1,
           [named](PointEvaluator3& e) { return named(e, QStringLiteral("nu")); });
    define(p + QStringLiteral("rho"), Dim::of(-3, 1), tr("density"), K::Derived, f, 0, 1,
           [named](PointEvaluator3& e) { return named(e, QStringLiteral("rho")); });
    define(p + QStringLiteral("dT"), Dim::temperature(), tr("temperature above the strain-free one"), K::Derived, f, 0, 1,
           [named](PointEvaluator3& e) { return named(e, QStringLiteral("dT")); });
}

void Solution3::defineMagnetic(int f)
{
    using K = VariableDef::Kind;
    const QString p = a_fields[std::size_t(f)].tag + QLatin1Char('.');
    const Dim aDim = Dim::of(1, 1, -2, -1), bDim = Dim::of(0, 1, -2, -1), hDim = Dim::of(-1, 0, 0, 1);
    const char* axis[3] = {"x", "y", "z"};
    using S = detail3::MagneticState;
    struct Vec {
        const char* name;
        Dim dim;
        const char* what;
        int which;   // 0 A, 1 B, 2 H
    };
    const Vec vecs[] = {{"A", aDim, "magnetic vector potential", 0}, {"B", bDim, "magnetic flux density", 1}, {"H", hDim, "magnetic field", 2}};
    for (const Vec& v : vecs) {
        const int which = v.which;
        const auto get = [which](const S& st, int k) { return which == 0 ? st.A[k] : which == 1 ? st.B[k] : st.H[k]; };
        for (int k = 0; k < 3; ++k)
            define(p + QString::fromLatin1(v.name) + QString::fromLatin1(axis[k]), v.dim,
                   tr("%1, %2 component").arg(tr(v.what), QString::fromLatin1(axis[k])), K::Derived, f, 0, 1, [f, get, k](PointEvaluator3& e) {
                       S st;
                       return detail3::magneticStateAt(e, f, st) ? get(st, k) : NaN;
                   });
        if (which > 0)
            define(p + QStringLiteral("norm") + QString::fromLatin1(v.name), v.dim, tr("%1, norm").arg(tr(v.what)), K::Derived, f, 0, 1,
                   [f, get](PointEvaluator3& e) {
                       S st;
                       if (!detail3::magneticStateAt(e, f, st)) return NaN;
                       return std::sqrt(get(st, 0) * get(st, 0) + get(st, 1) * get(st, 1) + get(st, 2) * get(st, 2));
                   });
    }
    define(p + QStringLiteral("Wm"), Dim::of(-1, 1, -2), tr("magnetic energy density"), K::Derived, f, 0, 1, [f](PointEvaluator3& e) {
        S st;
        return detail3::magneticStateAt(e, f, st) ? st.energy : NaN;
    });
    define(p + QStringLiteral("mur"), Dim(), tr("relative permeability (a tensor's diagonal's mean; a B-H curve's B/(µ0 H) here)"), K::Derived, f, 0,
           1, [f](PointEvaluator3& e) {
               S st;
               return detail3::magneticStateAt(e, f, st) ? st.mur : NaN;
           });
    // The current density its sources give (the External Current Densities'
    // and the coils').
    for (int k = 0; k < 3; ++k) {
        const QString key = QStringLiteral("J") + QString::fromLatin1(axis[k]);
        define(p + key, Dim::current() / Dim::area(), tr("current density, %1 component (its sources')").arg(QString::fromLatin1(axis[k])), K::Derived, f, 0, 1,
               [f, key](PointEvaluator3& e) {
                   const auto& named = e.solution().coefficients()[std::size_t(f)].named;
                   const auto it = named.find(key);
                   const int d = e.domain();
                   if (it == named.end() || d < 0 || d >= int(it->second.size())) return NaN;
                   if (!it->second[std::size_t(d)].isValid()) return 0.0;
                   return e.eval(it->second[std::size_t(d)]);
               });
    }
}

void Solution3::defineSemiconductor(int f)
{
    using K = VariableDef::Kind;
    const Field3& field = a_fields[std::size_t(f)];
    const QString p = field.tag + QLatin1Char('.');
    const Dim conc = Dim::of(-3), energy = Dim::of(2, 1, -2), efield = Dim::voltage() / Dim::length(), jfield = Dim::current() / Dim::area();
    const char* const what[13] = {"electrostatic potential", "electron concentration", "hole concentration", "donor concentration", "acceptor concentration",
                                  "the carriers' heat (Joule and recombination: a heat physics' source)", "conduction band edge", "valence band edge",
                                  "electrons' quasi-Fermi level", "holes' quasi-Fermi level", "net recombination rate (SRH, Auger, direct)",
                                  "electrons' temperature (hot carriers'; else the lattice's)", "holes' temperature (hot carriers'; else the lattice's)"};
    int slot[13];
    for (int c = 0; c < 13; ++c) {
        const QString name = field.components.value(c);
        const Dim dim = c == 0 ? Dim::voltage() : c == 5 ? Dim::power() / Dim::of(3) : c == 10 ? Dim::of(-3, 0, -1) : c >= 11 ? Dim::temperature() : c >= 6 ? energy : conc;
        slot[c] = definePart(p + name, dim, tr(what[c]), f, c);
    }
    int e[3];
    const char* axis[3] = {"x", "y", "z"};
    for (int k = 0; k < 3; ++k)
        e[k] = definePart(p + QStringLiteral("E") + QString::fromLatin1(axis[k]), efield, tr("electric field, %1 component").arg(QString::fromLatin1(axis[k])), f, 0, k);
    for (int k = 0; k < 3; ++k) a_defs[std::size_t(e[k])].sign = -1;
    define(p + QStringLiteral("normE"), efield, tr("electric field, norm"), K::Derived, f, 0, 1, [e](PointEvaluator3& ev) {
        const double a = ev.value(e[0]), b = ev.value(e[1]), c = ev.value(e[2]);
        return std::sqrt(a * a + b * b + c * c);
    });
    // Its domain's constants (ni, dEc, Eg, µ...) at the point.
    auto named = [f](PointEvaluator3& ev, const char* key) {
        const auto& m = ev.solution().coefficients()[std::size_t(f)].named;
        const auto it = m.find(QString::fromLatin1(key));
        const int d = ev.domain();
        if (it == m.end() || d < 0 || d >= int(it->second.size()) || !it->second[std::size_t(d)].isValid()) return NaN;
        return ev.eval(it->second[std::size_t(d)]);
    };
    const int V = slot[0], n = slot[1], pp = slot[2], nd = slot[3], na = slot[4];
    define(p + QStringLiteral("Ndop"), conc, tr("net doping, Nd - Na"), K::Derived, f, 0, 1, [nd, na](PointEvaluator3& ev) { return ev.value(nd) - ev.value(na); });
    define(p + QStringLiteral("ni"), conc, tr("intrinsic carrier concentration"), K::Derived, f, 0, 1, [named](PointEvaluator3& ev) { return named(ev, "ni"); });
    // (The bands and quasi-Fermi levels: the solver's own by node - a
    // heterojunction's, narrowing's -, against the reference semiconductor's
    // intrinsic level at V = 0.)
    constexpr double Qe = 1.602176634e-19, KB = 1.380649e-23;
    (void)V;
    // The currents' densities from the carriers' gradients and the field
    // (linear between the nodes: the edges' own currents are Scharfetter
    // and Gummel's).
    int jn[3], jp[3];
    for (int k = 0; k < 3; ++k) {
        jn[k] = define(p + QStringLiteral("Jn") + QString::fromLatin1(axis[k]), jfield, tr("electron current density, %1 component").arg(QString::fromLatin1(axis[k])),
                       K::Derived, f, 0, 1, [n, e, k, f, named](PointEvaluator3& ev) {
                           const auto& el = ev.solution().coefficients()[std::size_t(f)].elements;
                           const auto it = el.find(QStringLiteral("mun"));
                           const double mu = it != el.end() && ev.prism() >= 0 && ev.prism() < int(it->second.size()) ? it->second[std::size_t(ev.prism())] : named(ev, "mun");
                           const double vt = KB * named(ev, "T") / Qe;
                           double u = 0, g[3];
                           if (!ev.field(f, 1, u, g)) return NaN;
                           return Qe * mu * (ev.value(n) * ev.value(e[k]) + vt * g[k]);
                       });
        jp[k] = define(p + QStringLiteral("Jp") + QString::fromLatin1(axis[k]), jfield, tr("hole current density, %1 component").arg(QString::fromLatin1(axis[k])),
                       K::Derived, f, 0, 1, [pp, e, k, f, named](PointEvaluator3& ev) {
                           const auto& el = ev.solution().coefficients()[std::size_t(f)].elements;
                           const auto it = el.find(QStringLiteral("mup"));
                           const double mu = it != el.end() && ev.prism() >= 0 && ev.prism() < int(it->second.size()) ? it->second[std::size_t(ev.prism())] : named(ev, "mup");
                           const double vt = KB * named(ev, "T") / Qe;
                           double u = 0, g[3];
                           if (!ev.field(f, 2, u, g)) return NaN;
                           return Qe * mu * (ev.value(pp) * ev.value(e[k]) - vt * g[k]);
                       });
    }
    for (int k = 0; k < 3; ++k)
        define(p + QStringLiteral("J") + QString::fromLatin1(axis[k]), jfield, tr("current density, %1 component").arg(QString::fromLatin1(axis[k])), K::Derived, f, 0, 1,
               [jn, jp, k](PointEvaluator3& ev) { return ev.value(jn[k]) + ev.value(jp[k]); });
    define(p + QStringLiteral("normJ"), jfield, tr("current density, norm"), K::Derived, f, 0, 1, [jn, jp](PointEvaluator3& ev) {
        double s = 0;
        for (int k = 0; k < 3; ++k) s += std::pow(ev.value(jn[k]) + ev.value(jp[k]), 2);
        return std::sqrt(s);
    });
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
    s.frequency = a_frequency;
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
    a_frequency = s.frequency;
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

bool PointEvaluator3::field(int f, int part, double& u, double g[3])
{
    // (A field's values its nodes' by component, then any of its own after
    // them - a semiconductor's contacts' voltages and charges.)
    u = NaN;
    g[0] = g[1] = g[2] = NaN;
    if (f < 0 || f >= int(a_s.fields().size()) || a_prism < 0) return false;
    const Field3& field = a_s.fields()[std::size_t(f)];
    if (!field.space || a_domain < 0 || a_domain >= int(field.domains.size()) || !field.domains[std::size_t(a_domain)]
        || field.values.size() < std::size_t(field.space->size()) * std::size_t(field.count()) || part < 0 || part >= field.count())
        return false;
    const Space3& sp = *field.space;
    PrismShape s;
    prismShapeAt(sp.order(), a_xi, a_eta, a_zeta, s);
    const Space3::Map m = sp.map(a_prism, a_xi, a_eta, a_zeta);
    const int* d = sp.dofs(a_prism);
    const std::size_t offset = std::size_t(part) * std::size_t(sp.size());
    u = 0;
    g[0] = g[1] = g[2] = 0;
    for (int i = 0; i < s.count; ++i) {
        const double v = field.values[offset + std::size_t(d[i])];
        double gi[3];
        Space3::gradient(m, s.dxi[i], s.deta[i], s.dzeta[i], gi);
        u += s.n[i] * v;
        for (int k = 0; k < 3; ++k) g[k] += gi[k] * v;
    }
    if (f == a_perturbed && part == 0) u += a_perturbation;
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
        if (field(d.field, d.part, u, g)) {
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
                if (log) *log << tr("%1: %2 unknowns, %3 LU (Newton's matrix)").arg(name).arg(a_n).arg(a_lu->name().chopped(3));
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
                        || (solver != QLatin1String("iterative") && threadedCholesky() && rhs >= 6 + a_n / 20000
                            && a_n <= 300000);
    if (direct) {
        a_chol = std::make_unique<SparseCholesky>();
        a_chol->setOrdering(SparseCholesky::Ordering::Metis);
        a_chol->analyzePattern(a);
        a_chol->factorize(a);
        if (a_chol->info() == Eigen::Success) {
            if (log)
                *log << tr("%1: %2 unknowns, %3 (METIS order)")
                            .arg(name)
                            .arg(a_n)
                            .arg(a_chol->backend() == Backend::Strumpack ? a_chol->name() : tr("%1's Cholesky").arg(backendName(a_chol->backend())));
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
    if (a_nullCols > 0) a_amg->setNearNullspace(a_nullCols, a_nullB, a_blockSize);
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
        if (p->enabled && !fieldKind(p->type) && p->type != QLatin1String("eme") && p->type != QLatin1String("waves3")
            && p->type != QLatin1String("waves3t"))
            out << p->name();
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

constexpr double C0 = 299792458.0;

/// A port's name as a global's part: what is not a letter, a digit or _ made _.
QString globalPart(const QString& name)
{
    QString out;
    for (const QChar c : name) out += c.isLetterOrNumber() || c == QLatin1Char('_') ? c : QLatin1Char('_');
    return out;
}

/// An eigenmode expansion's globals at one wavelength: the wavelength, each
/// port mode's index, loss and TE fraction, the S-parameters of the fed
/// port modes' columns (by their numbers as S-parameter ports: eme.S21,
/// eme.S21dB...; eme.S_12_3 past nine), and the first fed one's R, T and
/// what is lost.
/// A full-wave solve's result as an expansion's is kept: its ports' modes,
/// its S-matrix, what each fed one leaves by, its log.
EmeResult asEmeResult(const Wave3Result& w)
{
    EmeResult r;
    r.frequency = w.frequency;
    for (const Wave3Result::PortMode& pm : w.portModes) r.portModes.push_back({pm.port, pm.label, pm.neff, pm.te});
    r.S = w.S;
    r.balance = w.balance;
    r.back = w.back;
    r.log = w.log;
    r.warnings = w.warnings;
    r.error = w.error;
    return r;
}

/// The device a full-wave solve's results are kept with: its ports' names
/// and whether each is fed.
EmeDevice deviceOfPorts(const Wave3Settings& st)
{
    EmeDevice d;
    d.fullWave = true;
    for (const Wave3Port& wp : st.ports) {
        EmePort p;
        p.name = wp.name;
        p.modes = wp.modes;
        p.excite = wp.excite;
        d.ports.push_back(p);
    }
    return d;
}

void setEmeGlobals(Solution3& s, const QString& tag, const EmeDevice& d, const EmeResult& r)
{
    const QString p = tag + QLatin1Char('.');
    s.setGlobal(p + QStringLiteral("lambda0"), C0 / r.frequency, Dim::length(), tr("free-space wavelength"));
    s.setGlobal(p + QStringLiteral("freq"), r.frequency, Dim::of(0, 0, -1), tr("frequency"));
    const int N = int(r.portModes.size());
    const double k0 = 2 * M_PI * r.frequency / C0;
    auto portOf = [&](int i) { return r.portModes[std::size_t(i)].port; };
    for (int i = 0; i < N; ++i) {
        const EmeResult::PortMode& pm = r.portModes[std::size_t(i)];
        const QString name = d.ports[std::size_t(pm.port)].name;
        const QString id = globalPart(name) + QLatin1Char('_') + pm.label;
        const QString of = tr("port %1's %2 (S-parameter port %3)").arg(name, pm.label).arg(i + 1);
        s.setGlobal(p + QStringLiteral("neff_") + id, pm.neff.real(), Dim(), tr("%1: its effective index").arg(of));
        s.setGlobal(p + QStringLiteral("alpha_") + id, -2 * k0 * pm.neff.imag(), Dim::of(-1), tr("%1: its power's loss per metre").arg(of));
        s.setGlobal(p + QStringLiteral("TEfraction_") + id, pm.te, Dim(), tr("%1: of its transverse |E|², the part across the axis in the plane").arg(of));
    }
    int first = -1;
    for (int j = 0; j < N; ++j) {
        if (!d.ports[std::size_t(portOf(j))].excite) continue;
        if (first < 0) first = j;
        for (int i = 0; i < N; ++i) {
            const std::complex<double> v = r.S(i, j);
            const QString base = N <= 9 ? p + QStringLiteral("S%1%2").arg(i + 1).arg(j + 1) : p + QStringLiteral("S_%1_%2").arg(i + 1).arg(j + 1);
            const QString what = tr("S-parameter %1 %2: into port %3's %4 of port %5's %6")
                                     .arg(i + 1)
                                     .arg(j + 1)
                                     .arg(d.ports[std::size_t(portOf(i))].name, r.portModes[std::size_t(i)].label)
                                     .arg(d.ports[std::size_t(portOf(j))].name, r.portModes[std::size_t(j)].label);
            s.setGlobal(base, v.real(), Dim(), what + tr(", its real part"));
            s.setGlobal(base + QStringLiteral("_im"), v.imag(), Dim(), what + tr(", its imaginary part"));
            s.setGlobal(base + QStringLiteral("dB"), 20 * std::log10(std::abs(v)), Dim(), what + tr(" in dB (of power, 10 log |S|²)"));
            s.setGlobal(base + QStringLiteral("_abs"), std::abs(v), Dim(), what + tr(", its magnitude"));
            s.setGlobal(base + QStringLiteral("_arg"), std::arg(v) * 180 / M_PI, Dim(), what + tr(", its phase in degrees"));
        }
    }
    if (first < 0) return;
    double R = 0, T = 0;
    for (int i = 0; i < N; ++i) (portOf(i) == portOf(first) ? R : T) += std::norm(r.S(i, first));
    const QString fed = tr("of port %1's %2 fed").arg(d.ports[std::size_t(portOf(first))].name, r.portModes[std::size_t(first)].label);
    s.setGlobal(p + QStringLiteral("R"), R, Dim(), tr("reflectance %1: the power back in its port's modes").arg(fed));
    s.setGlobal(p + QStringLiteral("T"), T, Dim(), tr("transmittance %1: the power out by the other ports' modes").arg(fed));
    // (A full wave's: over every mode carrying power at the ports, those
    // taken as S-parameter ports or not - its ports absorb, or their PMLs
    // take, them all: a window's own modes too, radiation reaching the
    // port's plane.)
    if (int(r.back.size()) == N && int(r.balance.size()) == N) {
        const double back = r.back[std::size_t(first)];
        s.setGlobal(p + QStringLiteral("Rall"), back, Dim(),
                    tr("reflectance %1 in every mode carrying power at its port, taken or not (its window's own, radiation, too)").arg(fed));
        s.setGlobal(p + QStringLiteral("Tall"), r.balance[std::size_t(first)] - back, Dim(),
                    tr("transmittance %1 in every mode carrying power at the other ports, taken or not (their windows' own, radiation, too)").arg(fed));
    }
    s.setGlobal(p + QStringLiteral("L"), 1 - R - T, Dim(), tr("what is lost %1: radiated, absorbed, or into modes no port takes").arg(fed));
}

/*!
 * A Wavelength Domain step: each eigenmode expansion it solves (its physics
 * of type "eme", all where none is chosen) at each wavelength - the
 * wavelengths side by side, each a snapshot of its globals. The devices
 * read once, at the first wavelength.
 */
bool wavelengthStep3(Solution3& s, const Node& step, const std::function<bool(double, const QString&)>& tell, double memoryLimit, QStringList* warnings,
                     QString* error)
{
    Dim dim;
    QString why;
    const std::optional<std::vector<double>> listed = valueList(step.text(QStringLiteral("wavelengths")), s.parameters().scope, &dim, &why);
    if (!listed) {
        *error = tr("%1: the wavelengths: %2").arg(step.name(), why);
        return false;
    }
    if (!dim.isNone() && !(dim == Dim::length()) && !(dim == Dim::of(0, 0, -1))) {
        *error = tr("%1: the wavelengths are in %2: give a length (1550[nm]), or a frequency in Hz").arg(step.name(), dimName(dim));
        return false;
    }
    std::vector<double> frequencies;
    for (double v : *listed) {
        const double f = dim == Dim::length() ? (v > 0 ? C0 / v : 0) : v;
        if (!(f > 0) || !std::isfinite(f)) {
            *error = tr("%1: a wavelength (or frequency) of %2: it must be more than 0").arg(step.name(), formatNumber(v));
            return false;
        }
        frequencies.push_back(f);
    }
    if (frequencies.empty()) {
        *error = tr("%1: give a wavelength at least").arg(step.name());
        return false;
    }
    const QStringList chosen = step.list(QStringLiteral("physics"));
    std::vector<const Node*> physics, waves;
    for (const Node* p : s.model().physics()) {
        if (!p->enabled || !(chosen.isEmpty() || chosen.contains(p->tag))) continue;
        if (p->type == QLatin1String("eme")) physics.push_back(p);
        if (p->type == QLatin1String("waves3") || p->type == QLatin1String("waves3t")) waves.push_back(p);
    }
    if (physics.empty() && waves.empty()) {
        *error = tr("%1: there is no eigenmode expansion or full-wave solve (Electromagnetic Waves, Eigenmode Expansion, Frequency "
                    "Domain or Time Domain) for it to solve")
                     .arg(step.name());
        return false;
    }
    const Topology3& topology = s.topology();
    // The full-wave solves: each wavelength after another, each its
    // factorization on every core - their fields' |E| and real parts at the
    // mesh's nodes kept, of the first port mode fed.
    const int WP = int(waves.size());
    std::vector<Wave3Settings> waveSettings(static_cast<std::size_t>(WP));
    for (int k = 0; k < WP; ++k) {
        QString why;
        if (!readWave3Settings(s.model(), *waves[std::size_t(k)], topology, s.parameters(), &waveSettings[std::size_t(k)], &why)) {
            *error = tr("%1: %2").arg(waves[std::size_t(k)]->name(), why);
            return false;
        }
        waveSettings[std::size_t(k)].memoryLimit = memoryLimit;
        waveSettings[std::size_t(k)].fieldsKept = 1;
        // (The ports' cuts of one mesh at every wavelength, the shortest's:
        // a sweep's S smooth, not its cuts' discretization jumping.)
        waveSettings[std::size_t(k)].portMeshWavelength = C0 / *std::max_element(frequencies.begin(), frequencies.end());
    }
    const int Wn = int(frequencies.size());
    std::vector<std::vector<EmeResult>> waveResults(static_cast<std::size_t>(WP));
    std::vector<std::vector<std::vector<std::array<std::complex<double>, 3>>>> waveFields(static_cast<std::size_t>(WP));
    std::shared_ptr<const Space3> nodes;
    if (WP > 0) nodes = std::make_shared<Space3>(s.meshPtr(), 1);
    {
        const double share = physics.empty() ? 1.0 : 0.5;
        bool stopped = false;
        for (int k = 0; k < WP && !stopped; ++k) {
            // A time domain's: one run for every wavelength.
            if (waves[std::size_t(k)]->type == QLatin1String("waves3t")) {
                Wave3Settings st = waveSettings[std::size_t(k)];
                const double from = share * double(k) / double(WP), span = share / double(WP);
                const QString name = waves[std::size_t(k)]->name();
                st.progress = [&](double f, const QString& what) {
                    if (tell && !tell(from + span * std::clamp(f, 0.0, 1.0), tr("%1: %2").arg(name, what))) stopped = true;
                };
                st.cancelled = [&] { return stopped; };
                const Wave3TimeResult t = solveWave3Time(s.model(), s.parametersPtr(), topology, s.meshPtr(), st, frequencies);
                if (!t.ok()) {
                    *error = stopped ? tr("cancelled") : tr("%1: %2").arg(name, t.error);
                    return false;
                }
                for (int w = 0; w < Wn; ++w) {
                    Wave3Result r = t.at[std::size_t(w)];
                    if (w == 0) {
                        r.log = t.log + r.log;
                        r.warnings = t.warnings + r.warnings;
                    }
                    waveFields[std::size_t(k)].push_back(r.fields.empty() ? std::vector<std::array<std::complex<double>, 3>>()
                                                                          : nodalField(*r.space, r.fields.front(), *nodes));
                    waveResults[std::size_t(k)].push_back(asEmeResult(r));
                }
                continue;
            }
            for (int w = 0; w < Wn && !stopped; ++w) {
                Wave3Settings st = waveSettings[std::size_t(k)];
                const double from = share * double(k * Wn + w) / double(WP * Wn), span = share / double(WP * Wn);
                const QString name = waves[std::size_t(k)]->name();
                st.progress = [&](double f, const QString& what) {
                    if (tell && !tell(from + span * std::clamp(f, 0.0, 1.0), tr("%1 at %2: %3").arg(name, formatQuantity(C0 / frequencies[std::size_t(w)], Dim::length()), what)))
                        stopped = true;
                };
                st.cancelled = [&] { return stopped; };
                Wave3Result r = solveWave3(s.model(), s.parametersPtr(), topology, s.meshPtr(), st, frequencies[std::size_t(w)]);
                if (!r.ok()) {
                    *error = stopped ? tr("cancelled")
                                     : tr("%1 at %2: %3").arg(name, formatQuantity(C0 / frequencies[std::size_t(w)], Dim::length()), r.error);
                    return false;
                }
                waveFields[std::size_t(k)].push_back(r.fields.empty() ? std::vector<std::array<std::complex<double>, 3>>()
                                                                      : nodalField(*r.space, r.fields.front(), *nodes));
                waveResults[std::size_t(k)].push_back(asEmeResult(r));
            }
        }
        if (stopped) {
            *error = tr("cancelled");
            return false;
        }
    }
    if (physics.empty()) {
        // Its fields: |E| and E's real parts (V/m, of 1 W fed), each a field
        // of its own, kept at each wavelength.
        const std::size_t firstField = s.fields().size();
        for (int k = 0; k < WP; ++k)
            for (const char* v : {"normE", "Ex", "Ey", "Ez"}) {
                Field3 f;
                f.tag = waves[std::size_t(k)]->tag;
                f.type = waves[std::size_t(k)]->type;
                f.variable = QString::fromLatin1(v);
                f.dim = Dim::voltage() / Dim::length();
                f.space = nodes;
                f.values.assign(std::size_t(nodes->size()), 0.0);
                f.domains.assign(topology.domains.size(), 1);
                f.solved = true;
                s.fields().push_back(std::move(f));
            }
        s.coefficients().resize(s.fields().size());
        s.defineVariables();
        for (int k = 0; k < WP; ++k) {
            Solution3::Optics o;
            o.physics = waves[std::size_t(k)]->tag;
            o.device = deviceOfPorts(waveSettings[std::size_t(k)]);
            o.fullWave = true;
            s.optics.push_back(o);
        }
        const std::size_t opticsFrom = s.optics.size() - std::size_t(WP);
        for (int w = 0; w < Wn; ++w) {
            for (int k = 0; k < WP; ++k) {
                const EmeResult& r = waveResults[std::size_t(k)][std::size_t(w)];
                const QString tag = waves[std::size_t(k)]->tag;
                setEmeGlobals(s, tag, s.optics[opticsFrom + std::size_t(k)].device, r);
                for (const QString& line : r.log) s.log << QStringLiteral("%1: %2").arg(waves[std::size_t(k)]->name(), line);
                for (const QString& wn : r.warnings) {
                    const QString said = QStringLiteral("%1: %2").arg(waves[std::size_t(k)]->name(), wn);
                    if (!warnings->contains(said)) *warnings << said;
                }
                s.optics[opticsFrom + std::size_t(k)].results.push_back(r);
                const auto& E = waveFields[std::size_t(k)][std::size_t(w)];
                for (int c = 0; c < 4; ++c) {
                    Field3& f = s.fields()[firstField + std::size_t(4 * k + c)];
                    for (std::size_t i = 0; i < f.values.size(); ++i) {
                        if (i >= E.size()) {
                            f.values[i] = 0;
                            continue;
                        }
                        f.values[i] = c == 0 ? std::sqrt(std::norm(E[i][0]) + std::norm(E[i][1]) + std::norm(E[i][2])) : E[i][std::size_t(c - 1)].real();
                    }
                }
            }
            s.setFrequency(frequencies[std::size_t(w)]);
            s.addSnapshot();
        }
        s.spectral = true;
        return true;
    }
    if (WP > 0) {
        *error = tr("%1: an eigenmode expansion and a full-wave solve in one step - give each a step of its own (its Physics)").arg(step.name());
        return false;
    }
    std::vector<EmeDevice> devices(physics.size());
    for (std::size_t k = 0; k < physics.size(); ++k)
        if (!readEmeDevice(s.model(), *physics[k], topology, s.parameters(), frequencies.front(), &devices[k], error)) return false;
    // Every (physics, wavelength), side by side: as many at once as the
    // step says, or the machine's cores halved (each one's cuts on the
    // rest, and Accelerate's threads within each).
    const int W = int(frequencies.size()), P = int(physics.size()), jobs = W * P;
    const int cores = std::max(1, int(std::thread::hardware_concurrency()));
    int atOnce = step.integer(QStringLiteral("parallel")) > 0 ? step.integer(QStringLiteral("parallel")) : std::max(1, cores / 2);
    atOnce = std::clamp(atOnce, 1, jobs);
    std::vector<EmeResult> results(static_cast<std::size_t>(jobs));
    std::atomic<int> next{0}, done{0};
    std::atomic<bool> stop{false};
    std::mutex telling;
    QElapsedTimer since;
    since.start();
    // (Asked by the mode solver before each of its products, on every
    // thread: Cancel's state read a tenth of a second apart at most.)
    auto cancelled = [&] {
        if (stop) return true;
        std::unique_lock<std::mutex> lock(telling, std::try_to_lock);
        if (!lock.owns_lock() || since.elapsed() < 100) return bool(stop);
        since.restart();
        if (tell && !tell(double(done) / jobs, step.name())) stop = true;
        return bool(stop);
    };
    auto work = [&] {
        for (int j = next++; j < jobs && !stop; j = next++) {
            EmeDevice d = devices[std::size_t(j % P)];
            d.settings.cancelled = cancelled;
            d.settings.threads = std::max(1, cores / atOnce);
            // (One wavelength alone: how far its cuts and its overlaps are, not
            // a number that stays put for the whole solve.)
            if (jobs == 1 && tell)
                d.settings.progress = [&](double f, const QString& what) {
                    std::lock_guard<std::mutex> lock(telling);
                    if (!tell(f, tr("%1: %2").arg(step.name(), what))) stop = true;
                };
            results[std::size_t(j)] = solveEme(s.model(), s.parametersPtr(), topology, d, frequencies[std::size_t(j / P)]);
            ++done;
            if (!results[std::size_t(j)].ok()) stop = true;
            {
                std::lock_guard<std::mutex> lock(telling);
                if (tell && !tell(double(done) / jobs, tr("%1: %2 of %3 wavelengths").arg(step.name()).arg(done / P).arg(W))) stop = true;
            }
        }
    };
    {
        std::vector<std::thread> pool;
        for (int t = 1; t < atOnce; ++t) pool.emplace_back(work);
        work();
        for (std::thread& t : pool) t.join();
    }
    for (int j = 0; j < jobs; ++j) {
        const EmeResult& r = results[std::size_t(j)];
        if (!r.error.isEmpty() && r.error != tr("cancelled") && r.error != QCoreApplication::translate("Eme3", "cancelled")) {
            *error = tr("%1 at %2: %3").arg(physics[std::size_t(j % P)]->name(), formatQuantity(C0 / frequencies[std::size_t(j / P)], Dim::length()), r.error);
            return false;
        }
    }
    if (stop || next < jobs) {
        *error = tr("cancelled");
        return false;
    }
    // Each wavelength a snapshot of its globals; the results kept.
    for (int k = 0; k < P; ++k) {
        Solution3::Optics o;
        o.physics = physics[std::size_t(k)]->tag;
        o.device = devices[std::size_t(k)];
        o.device.settings.cancelled = nullptr;
        s.optics.push_back(o);
    }
    const std::size_t opticsFrom = s.optics.size() - std::size_t(P);
    for (int w = 0; w < W; ++w) {
        for (int k = 0; k < P; ++k) {
            const EmeResult& r = results[std::size_t(w * P + k)];
            setEmeGlobals(s, physics[std::size_t(k)]->tag, devices[std::size_t(k)], r);
            for (const QString& line : r.log) s.log << QStringLiteral("%1: %2").arg(physics[std::size_t(k)]->name(), line);
            for (const QString& wn : r.warnings) {
                const QString said = QStringLiteral("%1: %2").arg(physics[std::size_t(k)]->name(), wn);
                if (!warnings->contains(said)) *warnings << said;
            }
            s.optics[opticsFrom + std::size_t(k)].results.push_back(r);
        }
        s.setFrequency(frequencies[std::size_t(w)]);
        s.addSnapshot();
    }
    s.spectral = true;
    return true;
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
/// A Frequency Domain step: each semiconductor's small signal about its
/// solution at rest, at each frequency a snapshot of its contacts'
/// admittance - <tag>.G_<k>_<l> and <tag>.C_<k>_<l> (its real part, and its
/// imaginary over ω), <tag>.Cqs... where both contacts' names are one letter
/// as Gkl, Ckl.
bool frequencyStep3(Problems3& problems, Solution3& s, const Node& node, const std::vector<int>& subset, QString* error)
{
    std::vector<int> semis;
    for (int i : subset)
        if (s.fields()[std::size_t(i)].type == QLatin1String("semiconductor")) semis.push_back(i);
    if (semis.empty()) {
        *error = tr("%1: a Frequency Domain step in 3D is a semiconductor's small signal - its physics have none").arg(node.name());
        return false;
    }
    Dim dim;
    QString why;
    const std::optional<std::vector<double>> listed = valueList(node.text(QStringLiteral("frequencies")), s.parameters().scope, &dim, &why);
    if (!listed || listed->empty()) {
        *error = tr("%1: its frequencies: %2").arg(node.name(), why.isEmpty() ? tr("none") : why);
        return false;
    }
    s.smallSignal = true;
    for (double f : *listed) {
        s.setFrequency(f);
        for (int i : semis) {
            std::vector<std::vector<std::complex<double>>> Y;
            QStringList names;
            if (!detail3::semiconductorSmallSignal3(*problems[std::size_t(i)], f, &Y, &names, error)) return false;
            problems[std::size_t(i)]->setGlobals();
            const QString tag = s.fields()[std::size_t(i)].tag + QLatin1Char('.');
            const double w = 2 * M_PI * f;
            for (int k = 0; k < int(names.size()); ++k)
                for (int l = 0; l < int(names.size()); ++l) {
                    const QString a = names[k], b = names[l];
                    const std::complex<double> y = Y[std::size_t(k)][std::size_t(l)];
                    s.setGlobal(tag + QStringLiteral("G_") + a + QLatin1Char('_') + b, y.real(), Dim::conductance(), tr("small-signal conductance, %1 %2").arg(a, b));
                    s.setGlobal(tag + QStringLiteral("C_") + a + QLatin1Char('_') + b, w > 0 ? y.imag() / w : 0.0, Dim::capacitance(),
                                tr("small-signal capacitance, %1 %2").arg(a, b));
                    if (a.size() == 1 && b.size() == 1) {
                        s.setGlobal(tag + QStringLiteral("G") + a + b, y.real(), Dim::conductance());
                        s.setGlobal(tag + QStringLiteral("C") + a + b, w > 0 ? y.imag() / w : 0.0, Dim::capacitance());
                    }
                }
        }
        s.setGlobal(QStringLiteral("freq"), f, Dim::of(0, 0, -1), tr("frequency"));
        s.addSnapshot();
    }
    s.log << tr("%1: the contacts' admittance at %2 frequencies").arg(node.name()).arg(listed->size());
    return true;
}

/// An Eigenfrequency step: its physics at rest solved first (in turn till
/// they agree), then its solid's \a count modes nearest its frequency, each
/// a snapshot of its eigenfrequency - the solid's field the mode (scaled to
/// a largest displacement of 1), its globals <tag>.freq and <tag>.omega.
bool eigenStep3(Problems3& problems, Solution3& s, const Node& node, const detail3::Options3& o, const std::vector<int>& subset,
                QString* error)
{
    std::vector<int> atRest, modal;
    for (int i : subset) (s.fields()[std::size_t(i)].type == QLatin1String("solid") ? modal : atRest).push_back(i);
    if (modal.size() != 1) {
        *error = modal.empty() ? tr("%1: an Eigenfrequency step in 3D finds a solid's modes - its physics have no solid mechanics").arg(node.name())
                               : tr("%1: an Eigenfrequency step in 3D finds one solid's modes at a time - choose one").arg(node.name());
        return false;
    }
    if (!atRest.empty()) {
        if (!solveTogether(problems, atRest, [&o](int) { return o; }, s, node.name(), error)) return false;
        for (int i : atRest) s.fields()[std::size_t(i)].solved = true;
    }
    const int count = std::clamp(node.integer(QStringLiteral("count")), 1, 200);
    double shift = 0;
    {
        QString why;
        const QString text = node.text(QStringLiteral("fshift")).trimmed();
        if (!text.isEmpty()) {
            const std::optional<double> v = evaluateConstant(text, s.parameters().scope, Dim::of(0, 0, -1), &why);
            if (!v) {
                *error = tr("%1: what it searches about: %2").arg(node.name(), why);
                return false;
            }
            shift = std::max(0.0, *v);
        }
    }
    const int fi = modal.front();
    detail3::Problem3& p = *problems[std::size_t(fi)];
    std::vector<double> frequencies;
    std::vector<std::vector<double>> modes;
    if (!p.eigenmodes(o, count, shift, &frequencies, &modes, error)) return false;
    Field3& f = s.fields()[std::size_t(fi)];
    f.solved = true;
    const QString tag = f.tag + QLatin1Char('.');
    s.eigen = true;
    for (std::size_t k = 0; k < frequencies.size(); ++k) {
        f.values = modes[k];
        s.setFrequency(frequencies[k]);
        for (int i : atRest) problems[std::size_t(i)]->setGlobals();
        p.setGlobals();
        s.setGlobal(tag + QStringLiteral("freq"), frequencies[k], Dim::of(0, 0, -1), tr("eigenfrequency"));
        s.setGlobal(tag + QStringLiteral("omega"), 2 * M_PI * frequencies[k], Dim::of(0, 0, -1), tr("angular eigenfrequency"));
        s.setGlobal(tag + QStringLiteral("mode"), double(k + 1), Dim(), tr("the mode's number, lowest first"));
        s.addSnapshot();
        s.log << tr("%1: mode %2 at %3").arg(node.name()).arg(k + 1).arg(formatQuantity(frequencies[k], Dim::of(0, 0, -1), 6));
    }
    return true;
}

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
    // (A field that starts from its solution at rest - a semiconductor's -,
    // not yet solved: solved so at its first time.)
    for (int i : stepped)
        if (problems[std::size_t(i)]->solvedAtRestFirst() && !s.fields()[std::size_t(i)].solved) {
            detail3::Options3 o = atRest(i);
            o.mass = 0;
            o.history = nullptr;
            if (!problems[std::size_t(i)]->solve(o, error)) return false;
            s.fields()[std::size_t(i)].solved = true;
        }
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
        out.error = tr("%1: not solved in 3D yet - heat, electrostatics, electric currents, solid mechanics, magnetic fields and semiconductors are")
                        .arg(unsolved.join(QLatin1String(", ")));
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
    bool optics = false;
    for (const Node* p : s->model().physics()) {
        if (!p->enabled) continue;
        const FieldKind* k = fieldKind(p->type);
        // (An eigenmode expansion has no field on the mesh: its cuts' own,
        // solved by a Wavelength Domain step.)
        if (!k) {
            optics = optics || p->type == QLatin1String("eme") || p->type == QLatin1String("waves3") || p->type == QLatin1String("waves3t");
            continue;
        }
        Field3 f;
        f.tag = p->tag;
        f.type = p->type;
        f.variable = QString::fromLatin1(k->variable);
        if (p->type == QLatin1String("solid")) f.components = {QStringLiteral("u"), QStringLiteral("v"), QStringLiteral("w")};
        // (A semiconductor's potential and carriers, and its doping: of
        // order 1, its box method's.)
        if (p->type == QLatin1String("semiconductor"))
            f.components = {QStringLiteral("V"),  QStringLiteral("n"),  QStringLiteral("p"),   QStringLiteral("Nd"),  QStringLiteral("Na"), QStringLiteral("H"),
                            QStringLiteral("Ec"), QStringLiteral("Ev"), QStringLiteral("Efn"), QStringLiteral("Efp"), QStringLiteral("R"), QStringLiteral("Tn"),
                            QStringLiteral("Tp")};
        f.dim = k->dim;
        f.initial = k->initial;
        const int order = p->text(QStringLiteral("order")) == QLatin1String("2") && p->type != QLatin1String("semiconductor") ? 2 : 1;
        if (!spaces[order]) spaces[order] = std::make_shared<Space3>(mesh, order);
        f.space = spaces[order];
        f.values.assign(std::size_t(f.space->size()) * std::size_t(f.count()), f.initial);
        // (A magnetic field: A on edge elements of its order, then the
        // gauge's multiplier on the nodal space.)
        if (p->type == QLatin1String("magnetic")) {
            f.edges = std::make_shared<EdgeSpace3>(mesh, order);
            f.values.assign(std::size_t(f.edges->size()) + std::size_t(f.space->size()), 0.0);
        }
        f.domains.assign(mesh->topology->domains.size(), 0);
        s->fields().push_back(std::move(f));
    }
    if (s->fields().empty() && !optics) {
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
            if (f.edges) {
                Estimate3 e = estimateMagnetic3(mesh, order);
                e.physics = f.tag;
                if (const Node* p = s->model().find(f.tag)) e.name = p->name();
                s->log << tr("Estimated: %1").arg(estimateText(e));
                out.estimates.push_back(e);
                continue;
            }
            if (nonzeros[order] < 0) nonzeros[order] = patternNonzeros(*f.space);
            Estimate3 e = estimateOn(*f.space, nonzeros[order], solver, componentsOf(f.type));
            e.physics = f.tag;
            if (const Node* p = s->model().find(f.tag)) e.name = p->name();
            s->log << tr("Estimated: %1").arg(estimateText(e));
            out.estimates.push_back(e);
        }
        // A full-wave solve's: its edge elements' unknowns, its factorization's
        // memory.
        for (const Node* p : s->model().physics()) {
            if (!p->enabled || (p->type != QLatin1String("waves3") && p->type != QLatin1String("waves3t"))) continue;
            const int order = p->text(QStringLiteral("order")) == QLatin1String("1") ? 1 : 2;
            Estimate3 e = p->type == QLatin1String("waves3t") ? estimateWave3Time(mesh, order) : estimateWave3(mesh, order);
            e.physics = p->tag;
            e.name = p->name();
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
        const QString& type = s->fields()[std::size_t(i)].type;
        problems.push_back(type == QLatin1String("solid")           ? detail3::makeSolidProblem3(*s, i)
                           : type == QLatin1String("magnetic")      ? detail3::makeMagneticProblem3(*s, i)
                           : type == QLatin1String("semiconductor") ? detail3::makeSemiconductorProblem3(*s, i)
                                                                    : detail3::makeScalarProblem3(*s, i));
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
        if (step.type == QLatin1String("wavelength")) {
            const double from = double(steps) / stepCount, span = 1.0 / stepCount;
            if (progress && !progress(from, step.name())) {
                out.error = tr("cancelled");
                return out;
            }
            auto tell = [progress, from, span](double f, const QString& what) {
                return !progress || progress(from + span * std::clamp(f, 0.0, 1.0), what);
            };
            QString why;
            if (!wavelengthStep3(*s, step, tell, asked.force ? 0.0 : asked.limit, &out.warnings, &why)) {
                out.error = why;
                out.log = s->log;
                return out;
            }
            ++steps;
            continue;
        }
        const bool modal = step.type == QLatin1String("eigenfrequency");
        const bool small = step.type == QLatin1String("frequency");
        if (step.type != QLatin1String("stationary") && !inTime && !modal && !small) {
            out.error = tr("%1: a %2 step is not solved in 3D yet - a Stationary, a Time Dependent, an Eigenfrequency or a Wavelength Domain one is")
                            .arg(step.name(), step.kind() ? step.kind()->title : step.type);
            out.log = s->log;
            return out;
        }
        if (s->fields().empty()) {
            out.error = tr("%1: the model's physics are light's (an eigenmode expansion or a full-wave solve), which a Wavelength Domain step "
                           "solves (not a %2 step)")
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
        // (A Wavelength Domain step's fields after the physics' - its light's
        // |E| and E, solved: none of them a problem to solve again.)
        for (int i = 0; i < int(s->fields().size()) && i < int(problems.size()); ++i)
            if (chosen.isEmpty() || chosen.contains(s->fields()[std::size_t(i)].tag)) subset.push_back(i);
        if (subset.empty()) {
            out.error = tr("%1 solves none of the model's physics (%2)").arg(step.name(), chosen.join(QLatin1String(", ")));
            return out;
        }
        QString why;
        if (small) {
            if (!frequencyStep3(problems, *s, step, subset, &why)) {
                out.error = why;
                out.log = s->log;
                return out;
            }
        } else if (modal) {
            if (!eigenStep3(problems, *s, step, o, subset, &why)) {
                out.error = why;
                out.log = s->log;
                return out;
            }
        } else if (inTime) {
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
        out.error = optics && s->fields().empty() ? tr("%1 has no step to solve: add a Wavelength Domain step").arg(study->name())
                                                  : tr("%1 has no step to solve: add a Stationary step").arg(study->name());
        return out;
    }
    // The globals: those of the last output time, kept, or of the fields at
    // rest.
    // (An Eigenfrequency step's: its lowest mode.)
    if (s->snapshotCount() > 0) s->select(s->eigen ? 0 : s->snapshotCount() - 1);
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
