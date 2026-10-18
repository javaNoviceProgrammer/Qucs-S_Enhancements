/*
 * fem_magnetic.cpp - magnetic fields: the magnetic vector potential out of
 *                    the plane (Az; about an axis Aφ), ∇×(ν ∇×A) = J with
 *                    its coils, magnets and surface currents - at rest (a
 *                    B-H curve by Newton's method) or in the frequency
 *                    domain with its eddy currents jωσA, a conductor fed a
 *                    current and its voltage found; the coils swept for an
 *                    inductance or impedance matrix
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_problem.h"

#include <Eigen/Dense>

#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace qucs_s::fem {

namespace {

constexpr double Mu0 = 1.25663706127e-6;

} // namespace

// ------------------------------------------------------------------ BHCurve

std::shared_ptr<const BHCurve> BHCurve::make(std::vector<std::pair<double, double>> points, QString* error)
{
    auto fail = [&](const QString& why) -> std::shared_ptr<const BHCurve> {
        if (error) *error = why;
        return nullptr;
    };
    std::sort(points.begin(), points.end());
    if (points.empty() || points.front().first > 0) points.insert(points.begin(), {0.0, 0.0});
    if (points.front().first < 0) return fail(detail::tr("a B-H curve's B starts at 0 (it is the curve's first half)"));
    if (points.front().second != 0) return fail(detail::tr("a B-H curve passes through (0, 0): its H at B = 0 is %1").arg(points.front().second));
    if (points.size() < 2) return fail(detail::tr("a B-H curve needs a point besides (0, 0)"));
    auto c = std::make_shared<BHCurve>();
    for (const auto& [b, h] : points) {
        if (!std::isfinite(b) || !std::isfinite(h)) return fail(detail::tr("a B-H curve's point is not a number"));
        if (!c->a_b.empty() && (b <= c->a_b.back() || h <= c->a_h.back()))
            return fail(detail::tr("a B-H curve rises: B and H grow from point to point (at B = %1 T)").arg(b));
        c->a_b.push_back(b);
        c->a_h.push_back(h);
    }
    // Slopes as PCHIP's: weighted harmonic means within, the secants at the
    // ends - so H rises everywhere between the points.
    const int n = int(c->a_b.size());
    std::vector<double> d(std::size_t(n - 1)), w(std::size_t(n - 1));
    for (int k = 0; k + 1 < n; ++k) {
        w[std::size_t(k)] = c->a_b[std::size_t(k + 1)] - c->a_b[std::size_t(k)];
        d[std::size_t(k)] = (c->a_h[std::size_t(k + 1)] - c->a_h[std::size_t(k)]) / w[std::size_t(k)];
    }
    c->a_m.assign(std::size_t(n), 0);
    c->a_m[0] = d[0];
    c->a_m[std::size_t(n - 1)] = d[std::size_t(n - 2)];
    for (int k = 1; k + 1 < n; ++k) {
        const double w1 = 2 * w[std::size_t(k)] + w[std::size_t(k - 1)], w2 = w[std::size_t(k)] + 2 * w[std::size_t(k - 1)];
        c->a_m[std::size_t(k)] = (w1 + w2) / (w1 / d[std::size_t(k - 1)] + w2 / d[std::size_t(k)]);
    }
    // The energy to each point: ∫ H dB of each piece (a cubic's, exactly).
    c->a_w.assign(std::size_t(n), 0);
    for (int k = 0; k + 1 < n; ++k) {
        const double hk = w[std::size_t(k)];
        const double y0 = c->a_h[std::size_t(k)], y1 = c->a_h[std::size_t(k + 1)];
        const double m0 = c->a_m[std::size_t(k)], m1 = c->a_m[std::size_t(k + 1)];
        // ∫0^1 of h00, h10, h01, h11: 1/2, 1/12, 1/2, -1/12
        c->a_w[std::size_t(k + 1)] = c->a_w[std::size_t(k)] + hk * (0.5 * y0 + hk * m0 / 12 + 0.5 * y1 - hk * m1 / 12);
    }
    return c;
}

int BHCurve::segment(double b) const
{
    const auto it = std::upper_bound(a_b.begin(), a_b.end(), b);
    return std::clamp(int(it - a_b.begin()) - 1, 0, int(a_b.size()) - 2);
}

double BHCurve::H(double b) const
{
    if (b < 0) return -H(-b);
    if (b >= a_b.back()) return a_h.back() + (b - a_b.back()) / Mu0;
    const int k = segment(b);
    const double hk = a_b[std::size_t(k + 1)] - a_b[std::size_t(k)];
    const double t = (b - a_b[std::size_t(k)]) / hk;
    const double t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * a_h[std::size_t(k)] + (t3 - 2 * t2 + t) * hk * a_m[std::size_t(k)]
           + (-2 * t3 + 3 * t2) * a_h[std::size_t(k + 1)] + (t3 - t2) * hk * a_m[std::size_t(k + 1)];
}

double BHCurve::slope(double b) const
{
    if (b < 0) return slope(-b);
    if (b >= a_b.back()) return 1 / Mu0;
    const int k = segment(b);
    const double hk = a_b[std::size_t(k + 1)] - a_b[std::size_t(k)];
    const double t = (b - a_b[std::size_t(k)]) / hk;
    const double t2 = t * t;
    return ((6 * t2 - 6 * t) * a_h[std::size_t(k)] + (3 * t2 - 4 * t + 1) * hk * a_m[std::size_t(k)]
            + (-6 * t2 + 6 * t) * a_h[std::size_t(k + 1)] + (3 * t2 - 2 * t) * hk * a_m[std::size_t(k + 1)])
           / hk;
}

double BHCurve::nu(double b) const
{
    b = std::abs(b);
    if (b < 1e-9 * a_b[1]) return a_m[0];
    return H(b) / b;
}

double BHCurve::energy(double b) const
{
    b = std::abs(b);
    if (b >= a_b.back()) {
        const double x = b - a_b.back();
        return a_w.back() + a_h.back() * x + x * x / (2 * Mu0);
    }
    const int k = segment(b);
    const double hk = a_b[std::size_t(k + 1)] - a_b[std::size_t(k)];
    const double t = (b - a_b[std::size_t(k)]) / hk;
    const double t2 = t * t, t3 = t2 * t, t4 = t3 * t;
    // The basis functions' integrals from 0 to t.
    const double i00 = t - t3 + t4 / 2, i10 = t2 / 2 - 2 * t3 / 3 + t4 / 4, i01 = t3 - t4 / 2, i11 = -t3 / 3 + t4 / 4;
    return a_w[std::size_t(k)]
           + hk * (i00 * a_h[std::size_t(k)] + i10 * hk * a_m[std::size_t(k)] + i01 * a_h[std::size_t(k + 1)] + i11 * hk * a_m[std::size_t(k + 1)]);
}

namespace detail {

namespace {

using cd = std::complex<double>;

/// A B-H curve as a material or a feature gives it: a table of rows [B, H]
/// (numbers in T and A/m, or with units), or text "0 0; 0.5 120; 1 300".
std::shared_ptr<const BHCurve> curveOf(const QJsonValue& value, const Scope& scope, QString* error)
{
    std::vector<std::pair<double, double>> points;
    const Dim tesla = Dim::of(0, 1, -2, -1), perMetre = Dim::of(-1, 0, 0, 1);
    auto number = [&](const QJsonValue& v, const Dim& dim, double* out) {
        if (v.isDouble()) {
            *out = v.toDouble();
            return true;
        }
        QString why;
        const std::optional<double> c = evaluateConstant(v.toString(), scope, dim, &why);
        if (!c) {
            if (error) *error = tr("the B-H curve's %1: %2").arg(v.toString(), why);
            return false;
        }
        *out = *c;
        return true;
    };
    QJsonArray rows;
    if (value.isArray()) rows = value.toArray();
    else {
        static const QRegularExpression rowSplit(QStringLiteral("[;\\n]")), cellSplit(QStringLiteral("[\\s,]+"));
        for (const QString& row : value.toString().split(rowSplit, Qt::SkipEmptyParts)) {
            const QStringList cells = row.trimmed().split(cellSplit, Qt::SkipEmptyParts);
            if (cells.isEmpty()) continue;
            if (cells.size() != 2) {
                if (error) *error = tr("a B-H curve's row is B and H: %1").arg(row.trimmed());
                return nullptr;
            }
            rows.append(QJsonArray{cells[0], cells[1]});
        }
    }
    for (const QJsonValue& r : rows) {
        const QJsonArray row = r.toArray();
        if (row.size() < 2) {
            if (error) *error = tr("a B-H curve's row is B and H");
            return nullptr;
        }
        double b = 0, h = 0;
        if (!number(row.at(0), tesla, &b) || !number(row.at(1), perMetre, &h)) return nullptr;
        points.emplace_back(b, h);
    }
    if (points.empty()) {
        if (error) *error = tr("the B-H curve has no points");
        return nullptr;
    }
    return BHCurve::make(points, error);
}

/// A coil: a conductor fed a current (its distribution found, eddy
/// currents in it), or turns of wire (the current spread evenly).
struct CoilDef {
    /// What holds it: its current, its voltage, no net current, none between
    /// its ends (a single conductor's; turns are fed a current).
    enum Role { Current, Voltage, Floating, Grounded };
    QString name;
    bool single = true;
    Role role = Current;
    double current = 0;     // A (of the actual solution): fed (Current), else found
    double voltage = 0;     // V: held (Voltage)
    double turns = 1;
    QVector<int> domains;
    double area = 0;        // m²: its cross-section in the plane
    double S = 0;           // ∫ σ g dA (single): its current is S U
    double sigmaG2 = 0;     // ∫ σ g² w dA: its resistance is sigmaG2 / S²
    int eq = -1;            // its voltage's equation (in the frequency domain)
};

struct BoundaryDef {
    enum Kind { None, Fixed, Free };
    Kind kind = None;
    int feature = -1;
    Expression value;       // Fixed: A0 (0: Magnetic Insulation)
};

class MagneticProblem : public Problem
{
public:
    MagneticProblem(Solution& solution, int field) : Problem(solution, field) {}

    bool setup(QString* error) override;
    void initialize() override;
    bool solve(const SolveOptions& options, QString* error) override;
    void setGlobals() override;
    bool harmonic() const override { return true; }
    bool converged() const override { return a_converged; }

private:
    bool a_converged = true;
    /// Each element's terms at the field \a u (real; the stationary
    /// tangent and internal force at it): K (ν β·β, the tangent where a
    /// curve is), Fint (∫ H·β), Fext (∫ Je N, the magnets' in the frequency
    /// domain), M (∫ σ N N), the coils' Bw (∫ σ g N w) and Bu (∫ σ N dA).
    void elementTerms(const std::vector<double>& u, bool frequency, bool sources);
    bool buildEquations(bool frequency, QString* error);
    void dirichletValues();
    /// Stationary: Newton's steps (one when linear).
    bool solveStatic(const SolveOptions& o, QString* error);
    bool solveHarmonic(const SolveOptions& o, QString* error);
    /// The coils' S, area and resistance terms (σ may read the fields).
    void coilTerms();
    /// Its flux linkage Φ (Wb): ∫ A j w over coil \a k, j its current
    /// density per ampere.
    cd fluxLinkage(int k, const std::vector<double>& re, const std::vector<double>& im) const;
    /// The energy and losses of the field as it is (W: J, P: W).
    void energyAndLosses(bool frequency, double omega);
    double weightOf(QPointF x) const { return a_s.weight(x); }
    double gOf(QPointF x) const { return a_s.axisymmetric() ? (x.x() > 0 ? 1 / (2 * M_PI * x.x()) : 0.0) : 1.0; }

    // By domain.
    std::vector<Expression> a_mur, a_sigma, a_je, a_brx, a_bry;
    // An anisotropic µr in the plane: xx, xy, yx, yy (none: a_mur's alone).
    std::array<std::vector<Expression>, 4> a_murT;
    std::vector<std::shared_ptr<const BHCurve>> a_curves;
    std::vector<int> a_coilOf;
    std::vector<CoilDef> a_coils;
    std::vector<BoundaryDef> a_conditions;               // by boundary
    std::vector<std::vector<Expression>> a_surface;      // by boundary: surface currents Js
    bool a_nonlinear = false;
    bool a_varies = true;                                // a coefficient reads a field, t or freq

    // The equations.
    std::vector<int> a_eq;                               // by dof: equation, -1 none, -3 fixed
    std::vector<int> a_fixedBoundary;                    // by dof: the boundary that fixes it (-2: the axis)
    std::vector<double> a_fixed;                         // by dof: its value
    int a_neq = 0, a_ndofEq = 0;
    bool a_patternFrequency = false, a_patternReady = false;
    LinearSystem a_real;
    ComplexSystem a_complex;
    bool a_realFactorized = false;

    // Element terms.
    std::vector<double> a_K, a_Fint, a_Fext, a_M, a_Bw, a_Bu;
    struct EdgeTerm {
        int edge;
        double F[3];
    };
    std::vector<EdgeTerm> a_edges;

    // What was found.
    double a_energy = 0, a_losses = 0, a_omega = 0;
    bool a_frequency = false;
    std::vector<cd> a_voltage, a_flux, a_current;        // by coil
    std::vector<int> a_ports;                            // the coils fed a current: the matrix's, in its order
    QString a_reference;                                 // the coil the matrix is reduced against (its name)
    std::vector<std::vector<cd>> a_matrix;               // their Z (frequency) or L (stationary) matrix
    bool a_swept = false;
};

// ------------------------------------------------------------------ Setup

bool MagneticProblem::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field& f = field();
    const Model& model = a_s.model();
    const Topology& topo = a_s.topology();
    const std::size_t nd = topo.domains.size();
    QString why;
    const QVector<int> domains = resolveSelection(topo, model, Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(nd, 0);
    for (int d : domains) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    const std::vector<const Node*> material = materials();
    std::vector<const Node*> modelOf(nd, nullptr);
    std::vector<QStringList> jeTexts(nd);
    std::vector<QString> brx(nd), bry(nd);
    a_coilOf.assign(nd, -1);
    a_coils.clear();
    a_conditions.assign(topo.boundaries.size(), BoundaryDef());
    a_surface.assign(topo.boundaries.size(), {});
    // Magnetic Insulation is on the boundaries outside the physics only.
    auto inside = [&](int d) { return d >= 0 && f.domains[std::size_t(d)]; };
    int order = 0;
    for (const Node& feature : a_node->children) {
        ++order;
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k) continue;
        const QVector<int> picked = resolveSelection(topo, model, k->selection, feature.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(feature.name(), why));
        const QString t = feature.type;
        if (t == QLatin1String("magnetic/ampere")) {
            for (int d : picked) modelOf[std::size_t(d)] = &feature;
        } else if (t == QLatin1String("magnetic/current")) {
            const QString text = feature.text(QStringLiteral("Je"));
            if (!compile(text, feature, QStringLiteral("Je")).isValid()) continue;
            for (int d : picked)
                if (inside(d)) jeTexts[std::size_t(d)] << QLatin1Char('(') + text + QLatin1Char(')');
        } else if (t == QLatin1String("magnetic/coil")) {
            CoilDef c;
            c.name = feature.text(QStringLiteral("name")).trimmed();
            if (c.name.isEmpty()) c.name = feature.name();
            for (const CoilDef& other : a_coils)
                if (other.name == c.name) problem(tr("%1: another coil is named %2").arg(feature.name(), c.name));
            c.single = feature.text(QStringLiteral("conductor")) != QLatin1String("multi");
            const QString role = feature.text(QStringLiteral("role"));
            if (c.single && role == QLatin1String("voltage")) c.role = CoilDef::Voltage;
            else if (c.single && role == QLatin1String("floating")) c.role = CoilDef::Floating;
            else if (c.single && role == QLatin1String("grounded")) c.role = CoilDef::Grounded;
            if (c.role == CoilDef::Current) {
                const std::optional<double> i0 = evaluateConstant(feature.text(QStringLiteral("I0")), a_s.parameters().scope, Dim::current(), &why);
                if (!i0) {
                    problem(tr("%1: its current: %2").arg(feature.name(), why));
                    continue;
                }
                c.current = *i0;
            } else if (c.role == CoilDef::Voltage) {
                const std::optional<double> v0 = evaluateConstant(feature.text(QStringLiteral("V0")), a_s.parameters().scope, Dim::voltage(), &why);
                if (!v0) {
                    problem(tr("%1: its voltage: %2").arg(feature.name(), why));
                    continue;
                }
                c.voltage = *v0;
            }
            if (!c.single) {
                const std::optional<double> n = evaluateConstant(feature.text(QStringLiteral("N")), a_s.parameters().scope, Dim(), &why);
                if (!n || !(*n > 0)) {
                    problem(tr("%1: its number of turns: %2").arg(feature.name(), n ? tr("it must be more than 0") : why));
                    continue;
                }
                c.turns = *n;
            }
            for (int d : picked) {
                if (!inside(d)) continue;
                if (a_coilOf[std::size_t(d)] >= 0)
                    problem(tr("%1: domain %2 is in coil %3 already").arg(feature.name()).arg(d + 1).arg(a_coils[std::size_t(a_coilOf[std::size_t(d)])].name));
                a_coilOf[std::size_t(d)] = int(a_coils.size());
                c.domains << d;
            }
            if (c.domains.isEmpty()) problem(tr("%1 is on no domain of %2").arg(feature.name(), a_node->name()));
            a_coils.push_back(c);
        } else if (t == QLatin1String("magnetic/magnet")) {
            // Its remanent flux density along a direction.
            const QStringList dir = feature.pair(QStringLiteral("direction"));
            const std::optional<double> dx = evaluateConstant(dir.value(0), a_s.parameters().scope, Dim(), &why);
            const std::optional<double> dy = dx ? evaluateConstant(dir.value(1), a_s.parameters().scope, Dim(), &why) : std::nullopt;
            if (!dx || !dy || std::hypot(*dx, *dy) == 0) {
                problem(tr("%1: its direction: %2").arg(feature.name(), dx && dy ? tr("not (0, 0)") : why));
                continue;
            }
            const double len = std::hypot(*dx, *dy);
            const bool user = feature.text(QStringLiteral("source")) == QLatin1String("user");
            for (int d : picked) {
                if (!inside(d)) continue;
                QString br = user ? feature.text(QStringLiteral("Br")) : QString();
                if (!user) {
                    const Node* m = material[std::size_t(d)];
                    br = m ? materialProperty(*m, QStringLiteral("Br")) : QString();
                    if (br.trimmed().isEmpty()) {
                        problem(tr("%1: domain %2's material has no remanent flux density Br").arg(feature.name()).arg(d + 1));
                        continue;
                    }
                }
                if (!compile(br, feature, QStringLiteral("Br")).isValid()) continue;
                brx[std::size_t(d)] = QStringLiteral("(%1)*%2").arg(br, QString::number(*dx / len, 'g', 17));
                bry[std::size_t(d)] = QStringLiteral("(%1)*%2").arg(br, QString::number(*dy / len, 'g', 17));
            }
        } else if (k->selection == Level::Boundary) {
            BoundaryDef c;
            c.feature = order;
            if (t == QLatin1String("magnetic/insulation")) {
                c.kind = BoundaryDef::Fixed;
                c.value = Expression::compile(QStringLiteral("0"), a_s.pointScope());
                for (int b : picked) {
                    const Topology::Boundary& bd = topo.boundaries[std::size_t(b)];
                    if (inside(bd.up) != inside(bd.down)) a_conditions[std::size_t(b)] = c;
                }
                continue;
            }
            if (t == QLatin1String("magnetic/potential")) {
                c.kind = BoundaryDef::Fixed;
                c.value = compile(feature.text(QStringLiteral("A0")), feature, QStringLiteral("A0"), false);
            } else if (t == QLatin1String("magnetic/pmc")) {
                c.kind = BoundaryDef::Free;
            } else if (t == QLatin1String("magnetic/surfacecurrent")) {
                const Expression js = compile(feature.text(QStringLiteral("Js")), feature, QStringLiteral("Js"));
                if (js.isValid())
                    for (int b : picked) a_surface[std::size_t(b)].push_back(js);
                continue;
            } else {
                continue;
            }
            for (int b : picked) a_conditions[std::size_t(b)] = c;
        }
    }
    // Each domain's permeability (or curve) and conductivity.
    Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    a_mur.assign(nd, Expression());
    for (auto& list : a_murT) list.clear();
    a_sigma.assign(nd, Expression());
    a_je.assign(nd, Expression());
    a_brx.assign(nd, Expression());
    a_bry.assign(nd, Expression());
    a_curves.assign(nd, nullptr);
    a_nonlinear = false;
    for (int d = 0; d < int(nd); ++d) {
        if (!f.domains[std::size_t(d)]) continue;
        const Node* feat = modelOf[std::size_t(d)];
        if (!feat) {
            problem(tr("domain %1 has no Ampère's Law feature").arg(d + 1));
            continue;
        }
        const bool user = feat->text(QStringLiteral("source")) == QLatin1String("user");
        const bool bh = feat->text(QStringLiteral("constitutive")) == QLatin1String("bh");
        const Node* m = material[std::size_t(d)];
        const QString object = topo.objectNames.value(topo.domains[std::size_t(d)].owner);
        if (!user && !m) {
            problem(tr("domain %1 (%2) has no material: its relative permeability is needed").arg(d + 1).arg(object));
            continue;
        }
        if (bh) {
            QString cwhy;
            const QJsonValue table = user ? feat->value(QStringLiteral("BH")) : materialValue(*m, QStringLiteral("BH"));
            if (table.isUndefined() || table.isNull() || (table.isArray() && table.toArray().isEmpty())
                || (table.isString() && table.toString().trimmed().isEmpty())) {
                problem(user ? tr("%1: its B-H curve is empty").arg(feat->name())
                             : tr("domain %1 (%2): its material, %3, has no B-H curve").arg(d + 1).arg(object, m->name()));
                continue;
            }
            a_curves[std::size_t(d)] = curveOf(table, a_s.parameters().scope, &cwhy);
            if (!a_curves[std::size_t(d)]) {
                problem(tr("domain %1 (%2): %3").arg(d + 1).arg(object, cwhy));
                continue;
            }
            a_nonlinear = true;
        } else {
            // One, or a tensor in the plane (a laminated core's): the
            // feature's in the model's axes, a material's turned by its
            // orientation.
            QString mt[4];
            const QString aniso = user ? feat->text(QStringLiteral("anisotropy")) : QString();
            if (user && (aniso == QLatin1String("diagonal") || aniso == QLatin1String("symmetric"))) {
                mt[0] = feat->text(QStringLiteral("murxx"));
                mt[3] = feat->text(QStringLiteral("muryy"));
                mt[1] = mt[2] = aniso == QLatin1String("symmetric") ? feat->text(QStringLiteral("murxy")) : QStringLiteral("0");
            } else if (!user && isTensorText(materialText(*m, QStringLiteral("mur")))) {
                if (const std::optional<TensorText> t = materialTensor(*m, QStringLiteral("mur"))) {
                    mt[0] = t->c[0][0];
                    mt[1] = t->c[0][1];
                    mt[2] = t->c[1][0];
                    mt[3] = t->c[1][1];
                }
            }
            if (!mt[0].isEmpty()) {
                const char* keys[4] = {"murxx", "murxy", "murxy", "muryy"};
                for (int k = 0; k < 4; ++k) {
                    a_murT[std::size_t(k)].resize(nd);
                    a_murT[std::size_t(k)][std::size_t(d)] = compile(mt[k].trimmed().isEmpty() ? QStringLiteral("0") : mt[k], user ? *feat : *m,
                                                                     user ? QString::fromLatin1(keys[k]) : QStringLiteral("mur"));
                }
                a_mur[std::size_t(d)] = Expression::compile(QStringLiteral("((%1) + (%2))/2").arg(mt[0], mt[3]), a_s.pointScope());
            } else {
                const QString text = user ? feat->text(QStringLiteral("mur")) : materialProperty(*m, QStringLiteral("mur"));
                if (text.trimmed().isEmpty()) {
                    problem(tr("domain %1 (%2): its material, %3, has no relative permeability").arg(d + 1).arg(object, m->name()));
                    continue;
                }
                const Expression e = compile(text, user ? *feat : *m, QStringLiteral("mur"));
                if (e.isValid() && e.isConstant() && !(e.constant() > 0)) problem(tr("domain %1: its relative permeability must be more than 0").arg(d + 1));
                a_mur[std::size_t(d)] = e;
            }
        }
        // Its conductivity (along z, where the currents are: a tensor's zz):
        // none given, none.
        QString sigma = user ? feat->text(QStringLiteral("sigma")) : (m ? materialProperty(*m, QStringLiteral("sigma")) : QString());
        if (!user && m && isTensorText(materialText(*m, QStringLiteral("sigma"))))
            if (const std::optional<TensorText> t = materialTensor(*m, QStringLiteral("sigma"))) sigma = t->c[2][2];
        const int coil = a_coilOf[std::size_t(d)];
        const bool turns = coil >= 0 && !a_coils[std::size_t(coil)].single;
        if (!sigma.trimmed().isEmpty() && !turns) {
            const Expression e = compile(sigma, user ? *feat : *m, QStringLiteral("sigma"));
            if (e.isValid() && !(e.isConstant() && e.constant() == 0)) a_sigma[std::size_t(d)] = e;
        }
        if (a_curves[std::size_t(d)] && !brx[std::size_t(d)].isEmpty())
            problem(tr("domain %1: a magnet of a B-H curve (give the magnet a relative permeability)").arg(d + 1));
        if (!brx[std::size_t(d)].isEmpty()) {
            a_brx[std::size_t(d)] = Expression::compile(brx[std::size_t(d)], a_s.pointScope());
            a_bry[std::size_t(d)] = Expression::compile(bry[std::size_t(d)], a_s.pointScope());
        }
        if (!jeTexts[std::size_t(d)].isEmpty()) a_je[std::size_t(d)] = Expression::compile(jeTexts[std::size_t(d)].join(QLatin1Char('+')), a_s.pointScope());
    }
    // The coils' areas; a coil of turns' current density.
    {
        const Space& sp = *f.space;
        const Mesh& mesh = a_s.mesh();
        const TriangleRule& rule = triangleRule(2);
        std::vector<double> area(nd, 0);
        for (int t = 0; t < sp.elements(); ++t)
            for (const auto& q : rule.points) area[std::size_t(mesh.domain[std::size_t(t)])] += q[2] * std::abs(sp.map(t, q[0], q[1]).det);
        for (CoilDef& c : a_coils) {
            c.area = 0;
            for (int d : c.domains) c.area += area[std::size_t(d)];
            if (!c.single && c.area > 0) {
                const QString j = QString::number(c.turns * c.current / c.area, 'g', 17) + QStringLiteral("[A/m^2]");
                for (int d : c.domains) {
                    const QString before = a_je[std::size_t(d)].isValid() ? a_je[std::size_t(d)].text() + QLatin1Char('+') : QString();
                    a_je[std::size_t(d)] = Expression::compile(before + j, a_s.pointScope());
                }
            }
            if (c.single)
                for (int d : c.domains)
                    if (!a_sigma[std::size_t(d)].isValid())
                        problem(tr("coil %1: domain %2 conducts nothing (no conductivity): a single conductor needs one").arg(c.name).arg(d + 1));
        }
    }
    // For the variables.
    coef.c = a_mur;
    coef.sources.assign(nd, {});
    coef.sourceScale.assign(nd, 1.0);
    coef.capacity.assign(nd, Expression());
    coef.curves = a_curves;
    coef.named[QStringLiteral("sigma")] = a_sigma;
    coef.named[QStringLiteral("Je")] = a_je;
    coef.named[QStringLiteral("Brx")] = a_brx;
    coef.named[QStringLiteral("Bry")] = a_bry;
    std::vector<Expression> coilIndex(nd);
    for (int d = 0; d < int(nd); ++d) {
        const int c = a_coilOf[std::size_t(d)];
        if (c >= 0 && a_coils[std::size_t(c)].single) coilIndex[std::size_t(d)] = Expression::compile(QString::number(c), a_s.pointScope());
    }
    coef.named[QStringLiteral("coil")] = coilIndex;
    const char* parts[4] = {"murxx", "murxy", "muryx", "muryy"};
    for (int k = 0; k < 4; ++k) {
        if (a_murT[std::size_t(k)].empty()) coef.named.erase(QString::fromLatin1(parts[k]));
        else coef.named[QString::fromLatin1(parts[k])] = a_murT[std::size_t(k)];
    }
    a_varies = false;
    for (const auto* list : {&a_mur, &a_sigma, &a_je, &a_brx, &a_bry, &a_murT[0], &a_murT[1], &a_murT[2], &a_murT[3]})
        for (const Expression& e : *list) a_varies = a_varies || (e.isValid() && a_s.varies(e));
    for (const auto& list : a_surface)
        for (const Expression& e : list) a_varies = a_varies || a_s.varies(e);
    // The reference coil of the reduced matrix: one fed a current.
    a_reference = a_node->text(QStringLiteral("reference")).trimmed();
    if (!a_reference.isEmpty()) {
        bool fed = false;
        for (const CoilDef& c : a_coils) fed = fed || (c.name == a_reference && c.role == CoilDef::Current);
        if (!fed) problem(tr("%1: its reference coil %2 is no coil fed a current (one of the coils' matrix)").arg(a_node->name(), a_reference));
    }
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    a_patternReady = false;
    a_realFactorized = false;
    return true;
}

void MagneticProblem::initialize()
{
    Field& f = field();
    f.values.assign(std::size_t(f.space->size()) * 2, 0.0);
}

// ------------------------------------------------------------------ Assembly

void MagneticProblem::coilTerms()
{
    const Field& f = a_s.fields()[std::size_t(a_fi)];
    const Space& sp = *f.space;
    const Mesh& mesh = a_s.mesh();
    const TriangleRule& rule = triangleRule(a_s.axisymmetric() ? 5 : 4);
    PointEvaluator ev(a_s);
    for (CoilDef& c : a_coils) {
        c.S = 0;
        c.sigmaG2 = 0;
    }
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = mesh.domain[std::size_t(t)];
        if (dom < 0 || a_coilOf[std::size_t(dom)] < 0) continue;
        CoilDef& c = a_coils[std::size_t(a_coilOf[std::size_t(dom)])];
        if (!c.single) continue;
        const Expression& sg = a_sigma[std::size_t(dom)];
        for (const auto& q : rule.points) {
            const Space::Map m = sp.map(t, q[0], q[1]);
            double sigma = 0;
            if (sg.isConstant()) sigma = sg.constant();
            else if (sg.isValid()) {
                ev.moveTo(t, q[0], q[1]);
                sigma = ev.eval(sg);
            }
            const double g = gOf(m.x), da = q[2] * std::abs(m.det);
            c.S += sigma * g * da;
            c.sigmaG2 += sigma * g * g * da * weightOf(m.x);
        }
    }
}

void MagneticProblem::elementTerms(const std::vector<double>& u, bool frequency, bool sources)
{
    const Field& fld = a_s.fields()[std::size_t(a_fi)];
    const Space& sp = *fld.space;
    const Mesh& mesh = a_s.mesh();
    const int per = sp.perElement();
    const int nt = sp.elements();
    const std::size_t pp = std::size_t(per * per);
    const bool axi = a_s.axisymmetric();
    a_K.assign(std::size_t(nt) * pp, 0);
    a_Fint.assign(frequency ? 0 : std::size_t(nt) * std::size_t(per), 0);
    a_Fext.assign(std::size_t(nt) * std::size_t(per), 0);
    a_M.assign(frequency ? std::size_t(nt) * pp : 0, 0);
    a_Bw.assign(std::size_t(nt) * std::size_t(per), 0);
    a_Bu.assign(std::size_t(nt) * std::size_t(per), 0);
    parallelFor(nt, [&](int begin, int end) {
        PointEvaluator ev(a_s);
        Shape s;
        double gx[6], gy[6], bx[6], by[6];
        for (int t = begin; t < end; ++t) {
            const int dom = mesh.domain[std::size_t(t)];
            if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
            const Expression& mur = a_mur[std::size_t(dom)];
            const Expression& sg = a_sigma[std::size_t(dom)];
            const Expression& je = a_je[std::size_t(dom)];
            const Expression& brx = a_brx[std::size_t(dom)];
            const Expression& bry = a_bry[std::size_t(dom)];
            const BHCurve* curve = a_curves[std::size_t(dom)].get();
            const int coil = a_coilOf[std::size_t(dom)];
            const bool single = coil >= 0 && a_coils[std::size_t(coil)].single;
            const int degree = sp.curved(t) || axi || curve ? 5 : (sp.order() == 1 ? 2 : 4);
            const TriangleRule& rule = triangleRule(degree);
            double* K = &a_K[std::size_t(t) * pp];
            double* Fi = frequency ? nullptr : &a_Fint[std::size_t(t) * std::size_t(per)];
            double* Fe = &a_Fext[std::size_t(t) * std::size_t(per)];
            double* M = frequency ? &a_M[std::size_t(t) * pp] : nullptr;
            double* Bw = &a_Bw[std::size_t(t) * std::size_t(per)];
            double* Bu = &a_Bu[std::size_t(t) * std::size_t(per)];
            const int* dofs = sp.dofs(t);
            for (const auto& q : rule.points) {
                shapeAt(sp.order(), q[0], q[1], s);
                const Space::Map m = sp.map(t, q[0], q[1]);
                const double da = q[2] * std::abs(m.det);
                const double w = da * weightOf(m.x);
                const double r = m.x.x();
                for (int i = 0; i < per; ++i) {
                    Space::gradient(m, s.dxi[i], s.deta[i], gx[i], gy[i]);
                    // β: the flux density of shape function i as A.
                    if (axi) {
                        bx[i] = -gy[i];
                        by[i] = gx[i] + (r > 0 ? s.n[i] / r : 0.0);
                    } else {
                        bx[i] = gy[i];
                        by[i] = -gx[i];
                    }
                }
                bool moved = false;
                auto at = [&](const Expression& e) {
                    if (e.isConstant()) return e.constant();
                    if (!moved) {
                        ev.moveTo(t, q[0], q[1]);
                        moved = true;
                    }
                    return ev.eval(e);
                };
                double sigma = sg.isValid() ? at(sg) : 0.0;
                if (!std::isfinite(sigma)) sigma = 0;
                // The flux density of u here (stationary).
                double b0 = 0, b1 = 0;
                if (!frequency)
                    for (int k = 0; k < per; ++k) {
                        const double v = u[std::size_t(dofs[k])];
                        b0 += bx[k] * v;
                        b1 += by[k] * v;
                    }
                double nu = 0, extra = 0;   // the tangent: nu β·β + extra (b·β_i)(b·β_j)
                // An anisotropic one: H = ν B, ν (µ0 µr)⁻¹ in the plane.
                double nut[4] = {0, 0, 0, 0};
                bool tensor = false;
                if (curve) {
                    const double b = std::hypot(b0, b1);
                    nu = curve->nu(b);
                    if (!frequency && b > 1e-12) extra = (curve->slope(b) - nu) / (b * b);
                } else if (!a_murT[0].empty() && a_murT[0][std::size_t(dom)].isValid()) {
                    double mt[4];
                    for (int k = 0; k < 4; ++k) mt[k] = at(a_murT[std::size_t(k)][std::size_t(dom)]);
                    const double det = mt[0] * mt[3] - mt[1] * mt[2];
                    nut[0] = mt[3] / (Mu0 * det);
                    nut[1] = -mt[1] / (Mu0 * det);
                    nut[2] = -mt[2] / (Mu0 * det);
                    nut[3] = mt[0] / (Mu0 * det);
                    tensor = true;
                } else {
                    nu = 1 / (Mu0 * at(mur));
                    nut[0] = nut[3] = nu;
                }
                if (!tensor) nut[0] = nut[3] = nu;
                double mx = 0, my = 0;   // the remanent flux density
                if (brx.isValid()) {
                    mx = at(brx);
                    my = at(bry);
                }
                const double jv = sources && je.isValid() ? at(je) : 0.0;
                const double g = gOf(m.x);
                for (int i = 0; i < per; ++i) {
                    const double bi = b0 * bx[i] + b1 * by[i];
                    for (int j = i; j < per; ++j) {
                        // β_i · ν β_j (ν symmetric)
                        double v = bx[i] * (nut[0] * bx[j] + nut[1] * by[j]) + by[i] * (nut[2] * bx[j] + nut[3] * by[j]);
                        if (extra != 0) v += extra * bi * (b0 * bx[j] + b1 * by[j]);
                        K[i * per + j] += w * v;
                        if (j != i) K[j * per + i] += w * v;
                        if (M && sigma != 0) {
                            const double mv = w * sigma * s.n[i] * s.n[j];
                            M[i * per + j] += mv;
                            if (j != i) M[j * per + i] += mv;
                        }
                    }
                    // H·β: ν (b - Br)·β (a magnet's ν linear)
                    const double hx = nut[0] * (b0 - mx) + nut[1] * (b1 - my), hy = nut[2] * (b0 - mx) + nut[3] * (b1 - my);
                    const double hmx = nut[0] * mx + nut[1] * my, hmy = nut[2] * mx + nut[3] * my;
                    if (Fi) Fi[i] += w * (hx * bx[i] + hy * by[i]);
                    else if (sources && (mx != 0 || my != 0)) Fe[i] += w * (hmx * bx[i] + hmy * by[i]);
                    if (jv != 0) Fe[i] += w * jv * s.n[i];
                    if (single) {
                        Bw[i] += w * sigma * g * s.n[i];
                        Bu[i] += da * sigma * s.n[i];
                    }
                }
            }
        }
    });
    // Surface currents, edge by edge.
    a_edges.clear();
    if (!sources) return;
    PointEvaluator ev(a_s);
    const int lineN = sp.order() + 1 + (axi ? 1 : 0);
    for (int e = 0; e < int(mesh.edges.size()); ++e) {
        const Mesh::Edge& me = mesh.edges[std::size_t(e)];
        const std::vector<Expression>& js = a_surface[std::size_t(me.boundary)];
        if (js.empty()) continue;
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
        EdgeTerm term{e, {0, 0, 0}};
        const int ne = sp.order() == 1 ? 2 : 3;
        for (const auto& q : lineRule(lineN)) {
            double jac = 0;
            const QPointF x = sp.edgePoint(e, q[0], &jac);
            const double s = same ? q[0] : 1 - q[0];
            const double xi = k == 0 ? s : k == 1 ? 1 - s : 0;
            const double eta = k == 0 ? 0 : k == 1 ? s : 1 - s;
            ev.moveTo(tri, xi, eta);
            double v = 0;
            for (const Expression& j : js) v += ev.eval(j);
            if (!std::isfinite(v)) continue;
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
            const double w = q[1] * jac * weightOf(x);
            for (int i = 0; i < ne; ++i) term.F[i] += w * v * N[i];
        }
        a_edges.push_back(term);
    }
}

bool MagneticProblem::buildEquations(bool frequency, QString* error)
{
    Field& fld = field();
    const Space& sp = *fld.space;
    const Mesh& mesh = a_s.mesh();
    const Topology& topo = a_s.topology();
    const int nd = sp.size();
    const int per = sp.perElement();
    a_eq.assign(std::size_t(nd), -1);
    a_fixedBoundary.assign(std::size_t(nd), -1);
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = mesh.domain[std::size_t(t)];
        if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
        for (int i = 0; i < per; ++i) a_eq[std::size_t(sp.dofs(t)[i])] = -2;
    }
    // The conditions in their features' order (a later one wins at a point
    // two share): fixed or free.
    std::vector<int> byOrder;
    for (int b = 0; b < int(a_conditions.size()); ++b)
        if (a_conditions[std::size_t(b)].kind != BoundaryDef::None) byOrder.push_back(b);
    std::stable_sort(byOrder.begin(), byOrder.end(),
                     [&](int a, int b) { return a_conditions[std::size_t(a)].feature < a_conditions[std::size_t(b)].feature; });
    std::vector<std::vector<int>> edgesOf(topo.boundaries.size());
    for (int e = 0; e < int(mesh.edges.size()); ++e) edgesOf[std::size_t(mesh.edges[std::size_t(e)].boundary)].push_back(e);
    bool anyFixed = false;
    for (int b : byOrder) {
        const BoundaryDef& c = a_conditions[std::size_t(b)];
        for (int e : edgesOf[std::size_t(b)])
            for (int dof : sp.edgeDofs(e)) {
                if (dof < 0 || a_eq[std::size_t(dof)] == -1) continue;
                if (c.kind == BoundaryDef::Fixed) {
                    a_eq[std::size_t(dof)] = -3;
                    a_fixedBoundary[std::size_t(dof)] = b;
                    anyFixed = true;
                } else {
                    a_eq[std::size_t(dof)] = -2;
                    a_fixedBoundary[std::size_t(dof)] = -1;
                }
            }
    }
    // About the axis: A = 0 on it.
    if (a_s.axisymmetric()) {
        const QRectF bb = mesh.bounds();
        const double tol = 1e-9 * std::max(bb.width(), bb.height()) * mesh.unitScale;
        for (int dof = 0; dof < nd; ++dof)
            if (a_eq[std::size_t(dof)] != -1 && std::abs(sp.point(dof).x()) <= tol) {
                a_eq[std::size_t(dof)] = -3;
                a_fixedBoundary[std::size_t(dof)] = -2;
                anyFixed = true;
            }
    }
    if (!anyFixed) {
        *error = tr("%1: nothing sets the magnetic potential (no Magnetic Insulation, no Magnetic Potential on a boundary): it is "
                    "not determined")
                     .arg(a_node->name());
        return false;
    }
    a_neq = 0;
    for (int dof = 0; dof < nd; ++dof)
        if (a_eq[std::size_t(dof)] == -2) a_eq[std::size_t(dof)] = a_neq++;
    a_ndofEq = a_neq;
    if (a_neq == 0) {
        *error = tr("%1: nothing to solve - every value is set").arg(a_node->name());
        return false;
    }
    // (A conductor's voltage, in the frequency domain, is not one of these:
    // it is eliminated - a row of it would make the matrix's factors dense.)
    auto elements = [&](const std::function<void(const int*, int)>& element) {
        int local[6];
        for (int t = 0; t < sp.elements(); ++t) {
            const int dom = mesh.domain[std::size_t(t)];
            if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
            for (int i = 0; i < per; ++i) local[i] = a_eq[std::size_t(sp.dofs(t)[i])];
            element(local, per);
        }
    };
    if (frequency) a_complex.setPattern(a_neq, elements);
    else a_real.setPattern(a_neq, elements);
    a_patternFrequency = frequency;
    a_patternReady = true;
    a_realFactorized = false;
    return true;
}

void MagneticProblem::dirichletValues()
{
    const Space& sp = *field().space;
    const int nd = sp.size();
    a_fixed.assign(std::size_t(nd), 0);
    std::vector<double> values(std::size_t(a_s.pointScope().slotCount()), NaN);
    const auto& defs = a_s.definitions();
    for (int dof = 0; dof < nd; ++dof) {
        const int b = a_fixedBoundary[std::size_t(dof)];
        if (a_eq[std::size_t(dof)] != -3 || b < 0) continue;
        const Expression& e = a_conditions[std::size_t(b)].value;
        double v = 0;
        if (e.isConstant()) v = e.constant();
        else if (e.isValid()) {
            const QPointF p = sp.point(dof);
            for (int s = 0; s < int(defs.size()); ++s) {
                const auto k = defs[std::size_t(s)].kind;
                values[std::size_t(s)] = k == Solution::VariableDef::X ? p.x() : k == Solution::VariableDef::Y ? p.y()
                                         : k == Solution::VariableDef::Time ? a_s.time() : k == Solution::VariableDef::Frequency ? a_s.frequency()
                                                                                                                               : NaN;
            }
            v = e.eval(values.data());
        }
        a_fixed[std::size_t(dof)] = std::isfinite(v) ? v : 0.0;
    }
}

// ------------------------------------------------------------------ Solving

bool MagneticProblem::solve(const SolveOptions& o, QString* error)
{
    Field& fld = field();
    if (int(fld.values.size()) != fld.space->size() * 2) initialize();
    const bool frequency = o.harmonic && o.frequency > 0;
    if (frequency && a_nonlinear) {
        *error = tr("%1: a B-H curve in the frequency domain - give its domains a relative permeability (Ampère's Law, "
                    "Relative permeability) for a study in frequency")
                     .arg(a_node->name());
        return false;
    }
    if (!a_patternReady || a_patternFrequency != frequency)
        if (!buildEquations(frequency, error)) return false;
    dirichletValues();
    coilTerms();
    a_frequency = o.harmonic;
    a_omega = 2 * M_PI * (o.harmonic ? o.frequency : 0.0);
    if (!(frequency ? solveHarmonic(o, error) : solveStatic(o, error))) return false;
    fld.solved = true;
    energyAndLosses(frequency, a_omega);
    return true;
}

bool MagneticProblem::solveStatic(const SolveOptions& o, QString* error)
{
    Field& fld = field();
    const Space& sp = *fld.space;
    const int nd = sp.size();
    const int per = sp.perElement();
    const std::size_t pp = std::size_t(per * per);
    const Mesh& mesh = a_s.mesh();
    std::vector<double> u(fld.values.begin(), fld.values.begin() + nd);
    for (int dof = 0; dof < nd; ++dof) {
        if (a_eq[std::size_t(dof)] == -3) u[std::size_t(dof)] = a_fixed[std::size_t(dof)];
        else if (a_eq[std::size_t(dof)] == -1 || !std::isfinite(u[std::size_t(dof)])) u[std::size_t(dof)] = 0;
    }
    // A single conductor's voltage is what drives its current at rest: its
    // current's (I / S), or held (a voltage over the length); floating or
    // grounded, none.
    std::vector<double> U(a_coils.size(), 0);
    a_current.assign(a_coils.size(), 0);
    for (std::size_t k = 0; k < a_coils.size(); ++k) {
        const CoilDef& c = a_coils[k];
        if (c.single && c.role == CoilDef::Voltage) U[k] = c.voltage / (a_s.axisymmetric() ? 1.0 : a_s.thickness());
        else if (c.single && c.role == CoilDef::Current && c.S > 0) U[k] = c.current / c.S;
        a_current[k] = c.single && c.role != CoilDef::Current ? c.S * U[k] : c.current;
    }
    // F - Fint at the element terms as they were made last.
    auto residual = [&](Eigen::VectorXd& r) {
        r = Eigen::VectorXd::Zero(a_ndofEq);
        for (int t = 0; t < sp.elements(); ++t) {
            const int dom = mesh.domain[std::size_t(t)];
            if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
            const int coil = a_coilOf[std::size_t(dom)];
            const double drive = coil >= 0 ? U[std::size_t(coil)] : 0.0;
            const int* dofs = sp.dofs(t);
            for (int i = 0; i < per; ++i) {
                const int qi = a_eq[std::size_t(dofs[i])];
                if (qi < 0) continue;
                r[qi] += a_Fext[std::size_t(t) * std::size_t(per) + std::size_t(i)] + drive * a_Bw[std::size_t(t) * std::size_t(per) + std::size_t(i)]
                         - a_Fint[std::size_t(t) * std::size_t(per) + std::size_t(i)];
            }
        }
        const int ne = sp.order() == 1 ? 2 : 3;
        for (const EdgeTerm& et : a_edges) {
            const std::array<int, 3> dofs = sp.edgeDofs(et.edge);
            for (int i = 0; i < ne; ++i) {
                const int qi = a_eq[std::size_t(dofs[std::size_t(i)])];
                if (qi >= 0) r[qi] += et.F[i];
            }
        }
    };
    const bool newton = a_nonlinear;
    const int iterations = newton ? std::max(1, o.maxIterations) : 1;
    bool converged = !newton;
    Eigen::VectorXd r;
    elementTerms(u, false, true);
    residual(r);
    double rnorm = r.norm();
    const double scale = std::max(rnorm, 1e-300);
    int steps = 0;
    for (int it = 0; it < iterations; ++it) {
        // The tangent matrix (K where linear): symmetric, positive definite
        // while H rises with B.
        if (!a_realFactorized || newton || a_varies) {
            a_real.clear();
            for (int t = 0; t < sp.elements(); ++t) {
                const int dom = mesh.domain[std::size_t(t)];
                if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
                const double* K = &a_K[std::size_t(t) * pp];
                const int* dofs = sp.dofs(t);
                for (int i = 0; i < per; ++i) {
                    const int qi = a_eq[std::size_t(dofs[i])];
                    if (qi < 0) continue;
                    for (int j = 0; j < per; ++j) {
                        const int qj = a_eq[std::size_t(dofs[j])];
                        if (qj >= 0) a_real.add(qi, qj, K[i * per + j]);
                    }
                }
            }
            if (!a_real.factorize(true, o.solver, o.tolerance, a_node->name(), &a_s.log, error)) return false;
            a_realFactorized = true;
        }
        Eigen::VectorXd delta;
        if (!a_real.solve(r, delta, a_node->name(), error)) return false;
        // Newton's step, halved while it makes the residual larger.
        double lambda = 1;
        std::vector<double> trial = u;
        Eigen::VectorXd rt;
        for (int halving = 0; halving < (newton ? 8 : 1); ++halving) {
            trial = u;
            for (int dof = 0; dof < nd; ++dof) {
                const int q = a_eq[std::size_t(dof)];
                if (q >= 0) trial[std::size_t(dof)] += lambda * delta[q];
            }
            if (!newton) break;
            elementTerms(trial, false, true);
            residual(rt);
            if (rt.norm() <= rnorm * (1 - 1e-4 * lambda) || rt.norm() < 1e-12 * scale) break;
            lambda /= 2;
        }
        double change = 0, size = 0;
        for (int dof = 0; dof < nd; ++dof) {
            const int q = a_eq[std::size_t(dof)];
            if (q < 0) continue;
            change = std::max(change, std::abs(trial[std::size_t(dof)] - u[std::size_t(dof)]));
            size = std::max(size, std::abs(trial[std::size_t(dof)]));
        }
        u = trial;
        steps = it + 1;
        if (!newton) {
            elementTerms(u, false, true);
            break;
        }
        r = rt;
        rnorm = r.norm();
        if (change <= o.tolerance * (size + 1e-300) || rnorm <= 1e-12 * scale) {
            converged = true;
            break;
        }
    }
    if (newton) {
        if (converged) a_s.log << tr("%1: Newton converged in %2 steps").arg(a_node->name()).arg(steps);
        else a_s.log << tr("Warning: %1: Newton's method did not converge in %2 steps").arg(a_node->name()).arg(iterations);
    }
    a_converged = converged;
    std::copy(u.begin(), u.end(), fld.values.begin());
    std::fill(fld.values.begin() + nd, fld.values.end(), 0.0);
    // Each coil: its flux linkage, its voltage (its resistance's).
    a_voltage.assign(a_coils.size(), 0);
    a_flux.assign(a_coils.size(), 0);
    const std::vector<double> zero(std::size_t(nd), 0.0);
    fld.terminals.clear();
    for (std::size_t k = 0; k < a_coils.size(); ++k) {
        const CoilDef& c = a_coils[k];
        a_flux[k] = fluxLinkage(int(k), u, zero);
        // (At rest a conductor's voltage is its resistance's: U over its length.)
        const double rdc = c.single && c.S > 0 ? c.sigmaG2 / (c.S * c.S) : 0.0;
        a_voltage[k] = rdc * a_current[k];
        Field::Terminal t;
        t.name = c.name;
        t.potential = U[k];
        t.charge = a_current[k].real();
        fld.terminals.push_back(t);
    }
    // The coils fed a current swept: each at 1 A, the others at none, no
    // other source - an inductance matrix (Φ_k of coil j's ampere).
    a_swept = false;
    a_matrix.clear();
    a_ports.clear();
    for (int k = 0; k < int(a_coils.size()); ++k)
        if (a_coils[std::size_t(k)].role == CoilDef::Current) a_ports.push_back(k);
    if (o.sweep && !a_ports.empty()) {
        if (a_nonlinear) {
            a_s.log << tr("%1: no inductance matrix of a B-H curve (each coil's L is its Φ / I at its current)").arg(a_node->name());
        } else {
            const int n = int(a_ports.size());
            a_matrix.assign(std::size_t(n), std::vector<cd>(std::size_t(n), 0));
            std::vector<double> keepFext = a_Fext;
            std::vector<EdgeTerm> keepEdges = a_edges;
            for (int jp = 0; jp < n; ++jp) {
                const int j = a_ports[std::size_t(jp)];
                const CoilDef& c = a_coils[std::size_t(j)];
                Eigen::VectorXd rhs = Eigen::VectorXd::Zero(a_ndofEq);
                for (int t = 0; t < sp.elements(); ++t) {
                    const int dom = mesh.domain[std::size_t(t)];
                    if (dom < 0 || !fld.domains[std::size_t(dom)] || a_coilOf[std::size_t(dom)] != j) continue;
                    // Its current density per ampere: N/area, or σ g / S.
                    const int* dofs = sp.dofs(t);
                    std::vector<double> local(std::size_t(per), 0);
                    if (c.single) {
                        for (int i = 0; i < per; ++i) local[std::size_t(i)] = c.S > 0 ? a_Bw[std::size_t(t) * std::size_t(per) + std::size_t(i)] / c.S : 0.0;
                    } else {
                        // ∫ N_i w over the element, times N / area.
                        Shape s;
                        const TriangleRule& rule = triangleRule(4);
                        for (const auto& q : rule.points) {
                            shapeAt(sp.order(), q[0], q[1], s);
                            const Space::Map m = sp.map(t, q[0], q[1]);
                            const double w = q[2] * std::abs(m.det) * weightOf(m.x);
                            for (int i = 0; i < per; ++i) local[std::size_t(i)] += w * s.n[i] * c.turns / c.area;
                        }
                    }
                    for (int i = 0; i < per; ++i) {
                        const int qi = a_eq[std::size_t(dofs[i])];
                        if (qi >= 0) rhs[qi] += local[std::size_t(i)];
                    }
                }
                Eigen::VectorXd x;
                if (!a_real.solve(rhs, x, a_node->name(), error)) return false;
                std::vector<double> a(std::size_t(nd), 0);
                for (int dof = 0; dof < nd; ++dof)
                    if (a_eq[std::size_t(dof)] >= 0) a[std::size_t(dof)] = x[a_eq[std::size_t(dof)]];
                for (int kp = 0; kp < n; ++kp) a_matrix[std::size_t(kp)][std::size_t(jp)] = fluxLinkage(a_ports[std::size_t(kp)], a, zero);
            }
            a_Fext = keepFext;
            a_edges = keepEdges;
            a_swept = true;
        }
    }
    return true;
}

bool MagneticProblem::solveHarmonic(const SolveOptions& o, QString* error)
{
    // (K + jωM) A - Σ U_k b_k = F, each conductor's S_k U_k - jω u_k·A = I_k:
    // A = X_F + Σ U_l X_l with (K + jωM) X_F = F and (K + jωM) X_l = b_l,
    // then the conductors' n equations for their U.
    Field& fld = field();
    const Space& sp = *fld.space;
    const int nd = sp.size();
    const int per = sp.perElement();
    const std::size_t pp = std::size_t(per * per);
    const Mesh& mesh = a_s.mesh();
    const double omega = a_omega;
    const cd jw(0, omega);
    const std::vector<double> zero(std::size_t(nd), 0.0);
    elementTerms(zero, true, true);
    a_complex.clear();
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = mesh.domain[std::size_t(t)];
        if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
        const double* K = &a_K[std::size_t(t) * pp];
        const double* M = &a_M[std::size_t(t) * pp];
        const int* dofs = sp.dofs(t);
        for (int i = 0; i < per; ++i) {
            const int qi = a_eq[std::size_t(dofs[i])];
            if (qi < 0) continue;
            for (int j = 0; j < per; ++j) {
                const int qj = a_eq[std::size_t(dofs[j])];
                if (qj >= 0) a_complex.add(qi, qj, cd(K[i * per + j], omega * M[i * per + j]));
            }
        }
    }
    // (K + jωM: complex symmetric, K positive definite.)
    if (!a_complex.factorize(a_node->name(), error, true)) return false;
    // Each single conductor's b (∫ σ g N w) and u (∫ σ N dA) by equation;
    // what the fixed values give its equation (jω u·A_fixed). Those whose
    // voltage is found (fed a current, floating) are the conductors; those
    // held at one (a voltage, grounded) a source: Σ U_l b_l.
    const std::size_t ncoils = a_coils.size();
    std::vector<int> conductors;
    for (int k = 0; k < int(ncoils); ++k) {
        const CoilDef& c = a_coils[std::size_t(k)];
        if (c.single && (c.role == CoilDef::Current || c.role == CoilDef::Floating)) conductors.push_back(k);
    }
    const int nc = int(conductors.size());
    std::vector<Eigen::VectorXcd> bvec(ncoils), uvec(ncoils);
    for (std::size_t k = 0; k < ncoils; ++k)
        if (a_coils[k].single) {
            bvec[k] = Eigen::VectorXcd::Zero(a_ndofEq);
            uvec[k] = Eigen::VectorXcd::Zero(a_ndofEq);
        }
    std::vector<cd> fixedPart(ncoils, 0);
    // The sources' right-hand side with the fixed values' part.
    Eigen::VectorXcd F = Eigen::VectorXcd::Zero(a_ndofEq);
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = mesh.domain[std::size_t(t)];
        if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
        const double* K = &a_K[std::size_t(t) * pp];
        const double* M = &a_M[std::size_t(t) * pp];
        const int* dofs = sp.dofs(t);
        const int coil = a_coilOf[std::size_t(dom)];
        const bool single = coil >= 0 && a_coils[std::size_t(coil)].single;
        for (int i = 0; i < per; ++i) {
            const int qi = a_eq[std::size_t(dofs[i])];
            const double bw = a_Bw[std::size_t(t) * std::size_t(per) + std::size_t(i)];
            const double bu = a_Bu[std::size_t(t) * std::size_t(per) + std::size_t(i)];
            if (qi >= 0) {
                F[qi] += a_Fext[std::size_t(t) * std::size_t(per) + std::size_t(i)];
                for (int j = 0; j < per; ++j)
                    if (a_eq[std::size_t(dofs[j])] == -3 && a_fixed[std::size_t(dofs[j])] != 0)
                        F[qi] -= cd(K[i * per + j], omega * M[i * per + j]) * a_fixed[std::size_t(dofs[j])];
            }
            if (single) {
                if (qi >= 0) {
                    bvec[std::size_t(coil)][qi] += bw;
                    uvec[std::size_t(coil)][qi] += bu;
                } else if (a_eq[std::size_t(dofs[i])] == -3) {
                    fixedPart[std::size_t(coil)] += jw * bu * a_fixed[std::size_t(dofs[i])];
                }
            }
        }
    }
    {
        const int ne = sp.order() == 1 ? 2 : 3;
        for (const EdgeTerm& et : a_edges) {
            const std::array<int, 3> dofs = sp.edgeDofs(et.edge);
            for (int i = 0; i < ne; ++i) {
                const int qi = a_eq[std::size_t(dofs[std::size_t(i)])];
                if (qi >= 0) F[qi] += et.F[i];
            }
        }
    }
    // A conductor's U per its voltage: over the length (round the axis: one turn).
    const double length = a_s.axisymmetric() ? 1.0 : a_s.thickness();
    std::vector<cd> held(ncoils, 0);
    for (std::size_t k = 0; k < ncoils; ++k)
        if (a_coils[k].single && a_coils[k].role == CoilDef::Voltage && a_coils[k].voltage != 0) {
            held[k] = a_coils[k].voltage / length;
            F += held[k] * bvec[k];
        }
    std::vector<Eigen::VectorXcd> X(static_cast<std::size_t>(nc));
    for (int l = 0; l < nc; ++l)
        if (!a_complex.solve(bvec[std::size_t(conductors[std::size_t(l)])], X[std::size_t(l)], a_node->name(), error)) return false;
    // The conductors' equations: (S δ - jω u_k·X_l) U = I + jω u_k·X_F.
    Eigen::MatrixXcd Mc = Eigen::MatrixXcd::Zero(nc, nc);
    for (int k = 0; k < nc; ++k) {
        const std::size_t ck = std::size_t(conductors[std::size_t(k)]);
        Mc(k, k) += a_coils[ck].S;
        for (int l = 0; l < nc; ++l) Mc(k, l) -= jw * (uvec[ck].transpose() * X[std::size_t(l)])(0);
    }
    Eigen::PartialPivLU<Eigen::MatrixXcd> mlu;
    if (nc > 0) mlu.compute(Mc);
    // A solution of sources F (fixed values and held voltages in it, or
    // not) and the conductors' currents; each single conductor's U and
    // current I = S U - jω u·A.
    auto solveFor = [this, &X, &conductors, &error, &fixedPart, &jw, &mlu, &nc, &ncoils, &nd, &uvec, &held](const Eigen::VectorXcd& rhs, const std::vector<cd>& currents, bool fixed, std::vector<double>& re, std::vector<double>& im,
                        std::vector<cd>& U, std::vector<cd>& I) {
        Eigen::VectorXcd xf;
        if (!a_complex.solve(rhs, xf, a_node->name(), error)) return false;
        Eigen::VectorXcd r(nc);
        for (int k = 0; k < nc; ++k) {
            const std::size_t ck = std::size_t(conductors[std::size_t(k)]);
            r[k] = currents[std::size_t(k)] + jw * (uvec[ck].transpose() * xf)(0) + (fixed ? fixedPart[ck] : cd(0));
        }
        Eigen::VectorXcd u = nc > 0 ? Eigen::VectorXcd(mlu.solve(r)) : Eigen::VectorXcd();
        for (int l = 0; l < nc; ++l) xf += u[l] * X[std::size_t(l)];
        re.assign(std::size_t(nd), 0);
        im.assign(std::size_t(nd), 0);
        for (int dof = 0; dof < nd; ++dof) {
            const int q = a_eq[std::size_t(dof)];
            if (q >= 0) {
                re[std::size_t(dof)] = xf[q].real();
                im[std::size_t(dof)] = xf[q].imag();
            } else if (q == -3 && fixed) {
                re[std::size_t(dof)] = a_fixed[std::size_t(dof)];
            }
        }
        U.assign(ncoils, 0);
        if (fixed) U = held;
        for (int k = 0; k < nc; ++k) U[std::size_t(conductors[std::size_t(k)])] = u[k];
        I.assign(ncoils, 0);
        for (std::size_t k = 0; k < ncoils; ++k)
            if (a_coils[k].single) I[k] = a_coils[k].S * U[k] - jw * (uvec[k].transpose() * xf)(0) - (fixed ? fixedPart[k] : cd(0));
        if (!xf.allFinite()) {
            *error = tr("%1: the solution is not finite").arg(a_node->name());
            return false;
        }
        return true;
    };
    // A coil's voltage: a conductor's U over the length (round the axis: U),
    // turns' jω Φ.
    auto voltageOf = [&](int k, const std::vector<double>& re, const std::vector<double>& im, const std::vector<cd>& U) {
        const CoilDef& c = a_coils[std::size_t(k)];
        if (c.single) return U[std::size_t(k)] * length;
        return jw * fluxLinkage(k, re, im);
    };
    std::vector<cd> currents(static_cast<std::size_t>(nc));
    for (int k = 0; k < nc; ++k) currents[std::size_t(k)] = a_coils[std::size_t(conductors[std::size_t(k)])].current;
    std::vector<double> re, im;
    std::vector<cd> U, I;
    if (!solveFor(F, currents, true, re, im, U, I)) return false;
    std::copy(re.begin(), re.end(), fld.values.begin());
    std::copy(im.begin(), im.end(), fld.values.begin() + nd);
    a_voltage.assign(ncoils, 0);
    a_flux.assign(ncoils, 0);
    a_current.assign(ncoils, 0);
    fld.terminals.clear();
    for (int k = 0; k < int(ncoils); ++k) {
        const CoilDef& c = a_coils[std::size_t(k)];
        a_flux[std::size_t(k)] = fluxLinkage(k, re, im);
        a_voltage[std::size_t(k)] = voltageOf(k, re, im, U);
        // (Fed: its current as given - found, it is the same to rounding.)
        a_current[std::size_t(k)] = c.single && c.role != CoilDef::Current ? I[std::size_t(k)] : cd(c.current);
        Field::Terminal t;
        t.name = c.name;
        t.potential = U[std::size_t(k)].real();
        t.potentialIm = U[std::size_t(k)].imag();
        t.charge = a_current[std::size_t(k)].real();
        fld.terminals.push_back(t);
    }
    // Swept: each coil fed a current at 1 A in turn, no other source (the
    // others as their roles hold them, a voltage at none) - the impedance
    // matrix of those.
    a_swept = false;
    a_matrix.clear();
    a_ports.clear();
    for (int k = 0; k < int(ncoils); ++k)
        if (a_coils[std::size_t(k)].role == CoilDef::Current) a_ports.push_back(k);
    if (o.sweep && !a_ports.empty()) {
        const int n = int(a_ports.size());
        a_matrix.assign(std::size_t(n), std::vector<cd>(std::size_t(n), 0));
        for (int jp = 0; jp < n; ++jp) {
            const int j = a_ports[std::size_t(jp)];
            const CoilDef& c = a_coils[std::size_t(j)];
            Eigen::VectorXcd rhs = Eigen::VectorXcd::Zero(a_ndofEq);
            std::vector<cd> cur(std::size_t(nc), 0);
            if (c.single) {
                for (int k = 0; k < nc; ++k)
                    if (conductors[std::size_t(k)] == j) cur[std::size_t(k)] = 1.0;
            } else {
                // Turns: their current density per ampere, N / area.
                Shape sh;
                const TriangleRule& rule = triangleRule(4);
                for (int t = 0; t < sp.elements(); ++t) {
                    const int dom = mesh.domain[std::size_t(t)];
                    if (dom < 0 || a_coilOf[std::size_t(dom)] != j) continue;
                    const int* dofs = sp.dofs(t);
                    for (const auto& q : rule.points) {
                        shapeAt(sp.order(), q[0], q[1], sh);
                        const Space::Map m = sp.map(t, q[0], q[1]);
                        const double w = q[2] * std::abs(m.det) * weightOf(m.x);
                        for (int i = 0; i < per; ++i) {
                            const int qi = a_eq[std::size_t(dofs[i])];
                            if (qi >= 0) rhs[qi] += w * sh.n[i] * c.turns / c.area;
                        }
                    }
                }
            }
            std::vector<double> rj, ij;
            std::vector<cd> Uj, Ij;
            if (!solveFor(rhs, cur, false, rj, ij, Uj, Ij)) return false;
            for (int kp = 0; kp < n; ++kp) a_matrix[std::size_t(kp)][std::size_t(jp)] = voltageOf(a_ports[std::size_t(kp)], rj, ij, Uj);
        }
        a_swept = true;
    }
    return true;
}

cd MagneticProblem::fluxLinkage(int k, const std::vector<double>& re, const std::vector<double>& im) const
{
    // ∫ A j w over the coil, j its current density per ampere: N / area for
    // turns, σ g / S for a conductor (as at rest).
    const CoilDef& c = a_coils[std::size_t(k)];
    const Field& fld = a_s.fields()[std::size_t(a_fi)];
    const Space& sp = *fld.space;
    const Mesh& mesh = a_s.mesh();
    const TriangleRule& rule = triangleRule(a_s.axisymmetric() ? 5 : 4);
    PointEvaluator ev(a_s);
    cd total = 0;
    Shape s;
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = mesh.domain[std::size_t(t)];
        if (dom < 0 || a_coilOf[std::size_t(dom)] != k) continue;
        const int* dofs = sp.dofs(t);
        const Expression& sg = a_sigma[std::size_t(dom)];
        for (const auto& q : rule.points) {
            shapeAt(sp.order(), q[0], q[1], s);
            const Space::Map m = sp.map(t, q[0], q[1]);
            const double w = q[2] * std::abs(m.det) * weightOf(m.x);
            cd a = 0;
            for (int i = 0; i < s.count; ++i) a += s.n[i] * cd(re[std::size_t(dofs[i])], im[std::size_t(dofs[i])]);
            double j = 0;
            if (c.single) {
                double sigma = 0;
                if (sg.isConstant()) sigma = sg.constant();
                else if (sg.isValid()) {
                    ev.moveTo(t, q[0], q[1]);
                    sigma = ev.eval(sg);
                }
                j = c.S > 0 ? sigma * gOf(m.x) / c.S : 0.0;
            } else {
                j = c.area > 0 ? c.turns / c.area : 0.0;
            }
            total += w * j * a;
        }
    }
    return total;
}

void MagneticProblem::energyAndLosses(bool frequency, double omega)
{
    const Field& fld = a_s.fields()[std::size_t(a_fi)];
    const Space& sp = *fld.space;
    const int nd = sp.size();
    const Mesh& mesh = a_s.mesh();
    const bool axi = a_s.axisymmetric();
    double energy = 0, losses = 0;
    PointEvaluator ev(a_s);
    Shape s;
    double gx[6], gy[6];
    for (int t = 0; t < sp.elements(); ++t) {
        const int dom = mesh.domain[std::size_t(t)];
        if (dom < 0 || !fld.domains[std::size_t(dom)]) continue;
        const TriangleRule& rule = triangleRule(sp.curved(t) || axi ? 5 : 4);
        const int* dofs = sp.dofs(t);
        const BHCurve* curve = a_curves[std::size_t(dom)].get();
        const int coil = a_coilOf[std::size_t(dom)];
        cd U = 0;
        if (coil >= 0 && coil < int(fld.terminals.size()))
            U = cd(fld.terminals[std::size_t(coil)].potential, fld.terminals[std::size_t(coil)].potentialIm);
        for (const auto& q : rule.points) {
            shapeAt(sp.order(), q[0], q[1], s);
            const Space::Map m = sp.map(t, q[0], q[1]);
            const double w = q[2] * std::abs(m.det) * weightOf(m.x);
            const double r = m.x.x();
            cd a = 0, b0 = 0, b1 = 0;
            for (int i = 0; i < s.count; ++i) {
                Space::gradient(m, s.dxi[i], s.deta[i], gx[i], gy[i]);
                const cd v(fld.values[std::size_t(dofs[i])], fld.values[std::size_t(nd + dofs[i])]);
                a += s.n[i] * v;
                if (axi) {
                    b0 += -gy[i] * v;
                    b1 += (gx[i] + (r > 0 ? s.n[i] / r : 0.0)) * v;
                } else {
                    b0 += gy[i] * v;
                    b1 += -gx[i] * v;
                }
            }
            bool moved = false;
            auto at = [&](const Expression& e) {
                if (e.isConstant()) return e.constant();
                if (!moved) {
                    ev.moveTo(t, q[0], q[1]);
                    moved = true;
                }
                return ev.eval(e);
            };
            const double bb = std::norm(b0) + std::norm(b1);
            if (curve) energy += w * curve->energy(std::sqrt(bb));
            else {
                // ν: isotropic, or the inverse of an anisotropic µr in the plane.
                double nut[4];
                if (!a_murT[0].empty() && a_murT[0][std::size_t(dom)].isValid()) {
                    double mt[4];
                    for (int k = 0; k < 4; ++k) mt[k] = at(a_murT[std::size_t(k)][std::size_t(dom)]);
                    const double det = Mu0 * (mt[0] * mt[3] - mt[1] * mt[2]);
                    nut[0] = mt[3] / det;
                    nut[1] = -mt[1] / det;
                    nut[2] = -mt[2] / det;
                    nut[3] = mt[0] / det;
                } else {
                    nut[0] = nut[3] = 1 / (Mu0 * at(a_mur[std::size_t(dom)]));
                    nut[1] = nut[2] = 0;
                }
                if (frequency)
                    energy += w * 0.25 * (std::conj(b0) * (nut[0] * b0 + nut[1] * b1) + std::conj(b1) * (nut[2] * b0 + nut[3] * b1)).real();
                else {
                    const double mx = a_brx[std::size_t(dom)].isValid() ? at(a_brx[std::size_t(dom)]) : 0.0;
                    const double my = a_bry[std::size_t(dom)].isValid() ? at(a_bry[std::size_t(dom)]) : 0.0;
                    const double dx = b0.real() - mx, dy = b1.real() - my;
                    energy += w * 0.5 * (dx * (nut[0] * dx + nut[1] * dy) + dy * (nut[2] * dx + nut[3] * dy));
                }
            }
            if (a_sigma[std::size_t(dom)].isValid()) {
                double sigma = at(a_sigma[std::size_t(dom)]);
                if (!std::isfinite(sigma)) sigma = 0;
                const cd e = gOf(m.x) * U - cd(0, omega) * a;
                losses += w * sigma * std::norm(e) * (frequency ? 0.5 : 1.0);
            }
        }
    }
    a_energy = energy;
    a_losses = losses;
}

// ------------------------------------------------------------------ Globals

void MagneticProblem::setGlobals()
{
    Solution& s = a_s;
    const Field& f = field();
    const QString p = f.tag + QLatin1Char('.');
    const Dim henry = Dim::of(2, 1, -2, -2), weber = Dim::of(2, 1, -2, -1);
    const bool harmonic = a_frequency && a_omega > 0;
    for (int k = 0; k < int(a_coils.size()) && k < int(a_voltage.size()) && k < int(a_current.size()); ++k) {
        const CoilDef& c = a_coils[std::size_t(k)];
        const QString n = c.name;
        const cd current = a_current[std::size_t(k)];
        const bool found = c.single && c.role != CoilDef::Current;
        s.setGlobal(p + QStringLiteral("I_") + n, current.real(), Dim::current(),
                    found ? (harmonic ? tr("coil %1's current, found (its role holds its voltage), its real part").arg(n)
                                      : tr("coil %1's current, found (its role holds its voltage)").arg(n))
                          : tr("coil %1's current").arg(n));
        if (harmonic && found)
            s.setGlobal(p + QStringLiteral("I_") + n + QStringLiteral("_im"), current.imag(), Dim::current(), tr("coil %1's current, its imaginary part").arg(n));
        s.setGlobal(p + QStringLiteral("V_") + n, a_voltage[std::size_t(k)].real(), Dim::voltage(),
                    harmonic ? tr("coil %1's voltage, its real part").arg(n) : tr("coil %1's voltage (its resistance's)").arg(n));
        s.setGlobal(p + QStringLiteral("Phi_") + n, a_flux[std::size_t(k)].real(), weber, tr("coil %1's flux linkage").arg(n));
        if (harmonic) {
            s.setGlobal(p + QStringLiteral("V_") + n + QStringLiteral("_im"), a_voltage[std::size_t(k)].imag(), Dim::voltage(),
                        tr("coil %1's voltage, its imaginary part").arg(n));
            const cd z = std::abs(current) > 0 ? a_voltage[std::size_t(k)] / current : cd(NaN, NaN);
            s.setGlobal(p + QStringLiteral("R_") + n, z.real(), Dim::resistance(), tr("coil %1's resistance, V/I's real part").arg(n));
            s.setGlobal(p + QStringLiteral("L_") + n, z.imag() / a_omega, henry, tr("coil %1's inductance, V/I's imaginary part over ω").arg(n));
            s.setGlobal(p + QStringLiteral("Z_") + n, std::abs(z), Dim::resistance(), tr("coil %1's impedance, |V/I|").arg(n));
        } else {
            const double rdc = c.single && c.S > 0 ? c.sigmaG2 / (c.S * c.S) : NaN;
            if (c.single) s.setGlobal(p + QStringLiteral("R_") + n, rdc, Dim::resistance(), tr("coil %1's resistance").arg(n));
            if (current.real() != 0)
                s.setGlobal(p + QStringLiteral("L_") + n, a_flux[std::size_t(k)].real() / current.real(), henry,
                            tr("coil %1's inductance, Φ / I (all the coils at their currents)").arg(n));
        }
    }
    if (a_swept) {
        const int n = int(a_matrix.size());
        // Each entry: L (the imaginary part over ω) and R (the real part; at
        // rest a conductor's own resistance on the diagonal, none off it).
        auto entry = [&](int i, int j, double* l, double* r) {
            const cd v = a_matrix[std::size_t(i)][std::size_t(j)];
            const CoilDef& ci = a_coils[std::size_t(a_ports[std::size_t(i)])];
            *l = harmonic ? v.imag() / a_omega : v.real();
            *r = harmonic ? v.real() : (i == j && ci.single && ci.S > 0 ? ci.sigmaG2 / (ci.S * ci.S) : 0.0);
        };
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                const QString a = a_coils[std::size_t(a_ports[std::size_t(i)])].name, b = a_coils[std::size_t(a_ports[std::size_t(j)])].name;
                double l = 0, res = 0;
                entry(i, j, &l, &res);
                const QString lName = p + QStringLiteral("L_") + a + QLatin1Char('_') + b;
                s.setGlobal(lName, l, henry, tr("inductance matrix, %1 %2").arg(a, b));
                if (a.size() == 1 && b.size() == 1) s.setGlobal(p + QStringLiteral("L") + a + b, l, henry, tr("inductance matrix, %1 %2").arg(a, b));
                s.setGlobal(p + QStringLiteral("R_") + a + QLatin1Char('_') + b, res, Dim::resistance(), tr("resistance matrix, %1 %2").arg(a, b));
                if (a.size() == 1 && b.size() == 1) s.setGlobal(p + QStringLiteral("R") + a + b, res, Dim::resistance(), tr("resistance matrix, %1 %2").arg(a, b));
            }
        // Against the reference coil: each current returning in it, the
        // voltage between them - Z_ij - Z_ir - Z_rj + Z_rr.
        int ref = -1;
        for (int i = 0; i < n; ++i)
            if (a_coils[std::size_t(a_ports[std::size_t(i)])].name == a_reference) ref = i;
        if (ref >= 0)
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    if (i == ref || j == ref) continue;
                    double l[4], r[4];
                    entry(i, j, &l[0], &r[0]);
                    entry(i, ref, &l[1], &r[1]);
                    entry(ref, j, &l[2], &r[2]);
                    entry(ref, ref, &l[3], &r[3]);
                    const QString a = a_coils[std::size_t(a_ports[std::size_t(i)])].name, b = a_coils[std::size_t(a_ports[std::size_t(j)])].name;
                    s.setGlobal(p + QStringLiteral("Lr_") + a + QLatin1Char('_') + b, l[0] - l[1] - l[2] + l[3], henry,
                                tr("inductance matrix against coil %1 (its return), %2 %3").arg(a_reference, a, b));
                    s.setGlobal(p + QStringLiteral("Rr_") + a + QLatin1Char('_') + b, r[0] - r[1] - r[2] + r[3], Dim::resistance(),
                                tr("resistance matrix against coil %1 (its return), %2 %3").arg(a_reference, a, b));
                }
    }
    s.setGlobal(p + QStringLiteral("W"), a_energy, Dim::of(2, 1, -2),
                harmonic ? tr("magnetic energy, its time average") : tr("magnetic energy"));
    s.setGlobal(p + QStringLiteral("P"), a_losses, Dim::power(), harmonic ? tr("resistive losses, their time average") : tr("resistive losses"));
}

} // namespace

std::unique_ptr<Problem> makeMagneticProblem(Solution& solution, int field)
{
    return std::make_unique<MagneticProblem>(solution, field);
}

} // namespace detail

} // namespace qucs_s::fem
