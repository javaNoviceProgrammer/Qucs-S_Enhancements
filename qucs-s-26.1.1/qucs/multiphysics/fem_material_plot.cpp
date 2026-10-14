/*
 * fem_material_plot.cpp - a material's properties along one variable
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_material_plot.h"

#include "fem_geometry.h"
#include "fem_materials.h"
#include "fem_problem.h"

#include <QCoreApplication>

#include <cmath>
#include <optional>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::MaterialPlot", text);
}

constexpr double Q = 1.602176634e-19, KB = 1.380649e-23;
const double NaN = std::numeric_limits<double>::quiet_NaN();

struct Info {
    const char* key;
    const char* label;
    const char* unit;
};

const std::vector<Info>& infos()
{
    static const std::vector<Info> list = {
        {"Eg", "band gap Eg", "eV"},
        {"chi", "electron affinity χ", "eV"},
        {"Nc", "conduction band's density of states Nc", "1/cm^3"},
        {"Nv", "valence band's density of states Nv", "1/cm^3"},
        {"ni", "intrinsic density ni", "1/cm^3"},
        {"epsilonr", "relative permittivity εr", ""},
        {"mun", "electron mobility µn", "cm^2/(V*s)"},
        {"mup", "hole mobility µp", "cm^2/(V*s)"},
        {"vsatn", "electrons' saturation velocity", "cm/s"},
        {"vsatp", "holes' saturation velocity", "cm/s"},
        {"taun", "electron SRH lifetime τn", "s"},
        {"taup", "hole SRH lifetime τp", "s"},
        {"caun", "electrons' Auger coefficient", "cm^6/s"},
        {"caup", "holes' Auger coefficient", "cm^6/s"},
        {"copt", "radiative coefficient B", "cm^3/s"},
        {"dEg", "band gap narrowing ΔEg", "eV"},
        {"alphan", "electrons' impact ionization rate αn", "1/cm"},
        {"alphap", "holes' impact ionization rate αp", "1/cm"},
        {"k", "thermal conductivity k", "W/(m*K)"},
        {"rho", "density ρ", "kg/m^3"},
        {"Cp", "heat capacity Cp", "J/(kg*K)"},
        {"n", "refractive index n", ""},
        {"kappa", "extinction coefficient κ", ""},
        {"alpha", "absorption coefficient 4πκ/λ", "1/cm"},
    };
    return list;
}

} // namespace

QStringList plottableProperties()
{
    QStringList out;
    for (const Info& i : infos()) out << QString::fromLatin1(i.key);
    return out;
}

MaterialCurves materialCurves(const Node& material, const QStringList& keys, const MaterialSweep& sweep, const Scope& parameters)
{
    MaterialCurves out;
    const QString v = sweep.variable;
    static const QStringList variables = {QStringLiteral("T"), QStringLiteral("N"), QStringLiteral("x"), QStringLiteral("lambda"), QStringLiteral("F")};
    if (!variables.contains(v)) {
        out.error = tr("the variable is T, N, x, lambda or F, not %1").arg(v);
        return out;
    }
    const int points = std::clamp(sweep.points, 2, 2001);
    if (!std::isfinite(sweep.from) || !std::isfinite(sweep.to) || (sweep.log && !(sweep.from > 0 && sweep.to > 0))) {
        out.error = tr("its range from %1 to %2 (more than 0 on a log scale)").arg(sweep.from).arg(sweep.to);
        return out;
    }
    if (v == QLatin1String("x") && detail::Problem::materialProperty(material, QStringLiteral("compound")).trimmed().isEmpty()) {
        out.error = tr("%1 is no alloy (its compound none): x is not its").arg(material.name());
        return out;
    }
    out.variable = v;
    out.unit = v == QLatin1String("T") ? QStringLiteral("K") : v == QLatin1String("N") ? QStringLiteral("1/cm^3")
             : v == QLatin1String("lambda") ? QStringLiteral("um") : v == QLatin1String("F") ? QStringLiteral("V/cm") : QString();
    const double scaleOf = v == QLatin1String("N") ? 1e-6 : v == QLatin1String("lambda") ? 1e6 : v == QLatin1String("F") ? 1e-2 : 1.0;
    // Which: as asked, else what it has.
    QStringList wanted = keys;
    const bool all = wanted.isEmpty();
    if (all) wanted = plottableProperties();
    for (const QString& k : wanted)
        if (!plottableProperties().contains(k)) {
            out.error = tr("%1 is not a property plotted (%2)").arg(k, plottableProperties().join(QStringLiteral(", ")));
            return out;
        }
    std::vector<MaterialCurve> curves;
    for (const QString& k : wanted)
        for (const Info& i : infos())
            if (k == QLatin1String(i.key)) curves.push_back({k, tr(i.label), QString::fromLatin1(i.unit), {}});
    QStringList models;
    auto said = [&models](const QString& m) {
        if (!models.contains(m)) models << m;
    };
    for (int p = 0; p < points; ++p) {
        const double s = double(p) / (points - 1);
        const double at = sweep.log ? sweep.from * std::pow(sweep.to / sweep.from, s) : sweep.from + s * (sweep.to - sweep.from);
        out.at.push_back(at * scaleOf);
        const double T = v == QLatin1String("T") ? at : sweep.T, N = v == QLatin1String("N") ? at : sweep.N;
        const double lambda = v == QLatin1String("lambda") ? at : sweep.lambda, F = v == QLatin1String("F") ? at : sweep.F;
        Node m = material;
        if (v == QLatin1String("x")) m.props.insert(QStringLiteral("x"), formatNumber(at, 12));
        else if (std::isfinite(sweep.x)) m.props.insert(QStringLiteral("x"), formatNumber(sweep.x, 12));
        if (std::isfinite(sweep.y)) m.props.insert(QStringLiteral("y"), formatNumber(sweep.y, 12));
        MaterialScope scope(parameters, T > 0 ? T : 300);
        scope.setConstant(QStringLiteral("lambda0"), lambda, Dim::length());
        auto text = [&](const QString& key) { return detail::Problem::materialProperty(m, key, &scope, T > 0 ? T : 300); };
        auto value = [&](const QString& key) -> std::optional<double> {
            const QString t = text(key);
            if (t.trimmed().isEmpty()) return std::nullopt;
            const Expression e = Expression::compile(t, scope);
            if (!e.isValid() || !e.isConstant()) return std::nullopt;
            return e.constant();
        };
        // (An energy: J, or eV as a plain number.)
        auto energy = [&](const QString& key) -> std::optional<double> {
            const QString t = text(key);
            if (t.trimmed().isEmpty()) return std::nullopt;
            const Expression e = Expression::compile(t, scope);
            if (!e.isValid() || !e.isConstant()) return std::nullopt;
            return e.dim() == Dim::of(2, 1, -2) ? e.constant() / Q : e.constant();
        };
        auto has = [&](const QString& key) { return !text(key).trimmed().isEmpty(); };
        // The mobility of carrier c (0 electrons): the lattice's, by doping as
        // the material gives it, saturated by the field.
        auto mobility = [&value, &has, &N, &said, &F](int c) -> double {
            const QString C = c == 0 ? QStringLiteral("n") : QStringLiteral("p");
            const std::optional<double> lattice = value(QStringLiteral("mu") + C);
            if (!lattice) return NaN;
            double mu = *lattice;
            if (has(QStringLiteral("nref") + C)) {
                const double mn = value(QStringLiteral("mu") + C + QStringLiteral("min")).value_or(0), nr = value(QStringLiteral("nref") + C).value_or(1);
                const double a = value(QStringLiteral("alpha") + C).value_or(1);
                mu = mn + (mu - mn) / (1 + std::pow(std::max(N, 0.0) / nr, a));
                said(tr("mobilities by doping Caughey and Thomas's"));
            } else if (has(QStringLiteral("cr") + C)) {
                const double m1 = value(QStringLiteral("m1") + C).value_or(0), m2 = value(QStringLiteral("m2") + C).value_or(0);
                const double u1 = value(QStringLiteral("mu1") + C).value_or(0), pc = value(QStringLiteral("pc") + C).value_or(0);
                const double cr = value(QStringLiteral("cr") + C).value_or(1), cs = value(QStringLiteral("cs") + C).value_or(1);
                const double a = value(QStringLiteral("m") + (c == 0 ? QStringLiteral("an") : QStringLiteral("ap"))).value_or(1);
                const double b = value(QStringLiteral("mb") + C).value_or(1);
                const double n = std::max(N, 1e-30);
                mu = m1 * std::exp(-pc / n) + (mu - m2) / (1 + std::pow(n / cr, a)) - u1 / (1 + std::pow(cs / n, b));
                said(tr("mobilities by doping Masetti's"));
            }
            if (F > 0 && has(QStringLiteral("vsat") + C)) {
                const double vs = value(QStringLiteral("vsat") + C).value_or(1e5), beta = value(QStringLiteral("beta") + C).value_or(c == 0 ? 2 : 1);
                mu /= std::pow(1 + std::pow(mu * F / vs, beta), 1 / beta);
                said(tr("saturated by the field (Caughey and Thomas's)"));
            }
            return mu;
        };
        for (MaterialCurve& curve : curves) {
            const QString k = curve.key;
            double y = NaN;
            if (k == QLatin1String("Eg") || k == QLatin1String("chi")) y = energy(k).value_or(NaN);
            else if (k == QLatin1String("Nc") || k == QLatin1String("Nv")) y = value(k).value_or(NaN) * 1e-6;
            else if (k == QLatin1String("ni")) {
                const double nc = value(QStringLiteral("Nc")).value_or(NaN), nv = value(QStringLiteral("Nv")).value_or(NaN);
                const double eg = energy(QStringLiteral("Eg")).value_or(NaN);
                y = std::sqrt(nc * nv) * std::exp(-eg * Q / (2 * KB * T)) * 1e-6;
            } else if (k == QLatin1String("mun") || k == QLatin1String("mup")) y = mobility(k == QLatin1String("mun") ? 0 : 1) * 1e4;
            else if (k == QLatin1String("vsatn") || k == QLatin1String("vsatp")) y = value(k).value_or(NaN) * 100;
            else if (k == QLatin1String("taun") || k == QLatin1String("taup")) {
                const QString C = k.right(1);
                y = value(k).value_or(NaN);
                if (has(QStringLiteral("N0") + C) && N > 0) {
                    const double n0 = value(QStringLiteral("N0") + C).value_or(1), a = value(QStringLiteral("fA")).value_or(1);
                    const double b = value(QStringLiteral("fB")).value_or(0), cc = value(QStringLiteral("fC")).value_or(1), e = value(QStringLiteral("fE")).value_or(1);
                    y /= a + b * N / n0 + cc * std::pow(N / n0, e);
                    said(tr("lifetimes by doping Fossum's"));
                }
            } else if (k == QLatin1String("caun") || k == QLatin1String("caup")) y = value(k).value_or(NaN) * 1e12;
            else if (k == QLatin1String("copt")) y = value(k).value_or(NaN) * 1e6;
            else if (k == QLatin1String("dEg")) {
                const std::optional<double> e = energy(QStringLiteral("Ebgn"));
                if (e) {
                    const double nr = value(QStringLiteral("Nbgn")).value_or(1), c = value(QStringLiteral("Cbgn")).value_or(0.5);
                    const double l = N > 0 ? std::log(N / nr) : -1e300;
                    // (Its small-N tail, E C/(√(l² + C) - l), without the cancelling.)
                    y = l > 0 ? *e * (l + std::sqrt(l * l + c)) : l > -1e100 ? *e * c / (std::sqrt(l * l + c) - l) : 0.0;
                }
            } else if (k == QLatin1String("alphan") || k == QLatin1String("alphap")) {
                const QString C = k.right(1);
                const std::optional<double> a = value(QStringLiteral("iia") + C), b = value(QStringLiteral("iib") + C);
                if (a && b && F > 0) {
                    const double beta = value(QStringLiteral("iibeta") + C).value_or(1), e0 = value(QStringLiteral("iiE0")).value_or(INFINITY);
                    const double a2 = value(QStringLiteral("iia") + C + QStringLiteral("2")).value_or(*a), b2 = value(QStringLiteral("iib") + C + QStringLiteral("2")).value_or(*b);
                    const bool high = F > e0;
                    y = (high ? a2 : *a) * std::exp(-std::pow((high ? b2 : *b) / F, beta)) * 1e-2;
                } else if (a && b) y = 0;
            } else if (k == QLatin1String("alpha")) y = 4 * M_PI * value(QStringLiteral("kappa")).value_or(NaN) / lambda * 1e-2;
            else y = value(k).value_or(NaN);
            curve.values.push_back(y);
        }
    }
    for (MaterialCurve& c : curves) {
        bool any = false;
        for (double y : c.values) any = any || std::isfinite(y);
        if (!any) {
            // (Said where it was asked for; all asked, those it has kept.)
            if (all) continue;
            out.notes << tr("%1: %2 none").arg(material.name(), c.label);
        }
        out.curves.push_back(std::move(c));
    }
    if (!models.isEmpty()) out.notes << tr("%1: %2").arg(material.name(), models.join(QStringLiteral(", ")));
    return out;
}

QString inheritedParameter(const Model& model, const Topology& topo, const Node& feature, const QString& key, QString* material)
{
    if (!feature.type.startsWith(QLatin1String("semiconductor/")) || feature.props.contains(key)
        || feature.text(QStringLiteral("source")) == QLatin1String("user"))
        return {};
    const QString mk = materialKeyOf(feature.type, key);
    const NodeKind* k = feature.kind();
    if (mk.isEmpty() || !k) return {};
    QString why;
    const QVector<int> domains = resolveSelection(topo, model, k->selection, feature.selection(), &why);
    if (domains.isEmpty()) return {};
    const Node* mat = nullptr;
    for (const Node& m : model.materials().children) {
        if (!m.enabled) continue;
        if (resolveSelection(topo, model, Level::Domain, m.selection(), &why).contains(domains.front())) mat = &m;
    }
    if (!mat) return {};
    const QString t = detail::Problem::materialProperty(*mat, mk);
    if (!t.trimmed().isEmpty() && material) *material = mat->name();
    return t;
}

} // namespace qucs_s::fem
