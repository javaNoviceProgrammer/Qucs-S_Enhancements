/*
 * fem_quantum.cpp - quantum wells: the effective-mass Schrödinger equation
 *                   -∇·(ħ²/2m ∇ψ) + V ψ = E ψ of electrons, heavy holes and
 *                   light holes (BenDaniel and Duke's: 1/m ∂ψ/∂n continuous
 *                   across a heterojunction), in 1D along the growth axis
 *                   through the layers (quadratic elements of their own) or
 *                   in 2D over the cross-section (wires; dots about an
 *                   axis); its bands from the materials' compounds (strained
 *                   on a substrate: Bir and Pikus), a field along the axis
 *                   (the quantum-confined Stark effect) or another physics'
 *                   potential; each carrier's lowest states by a shift and
 *                   invert below them; the transitions' energies and
 *                   overlaps, and an optical spectrum - absorption, or gain
 *                   with carriers in the wells
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_edge.h"
#include "fem_materials.h"
#include "fem_problem.h"
#include "fem_results.h"

#include <Eigen/Dense>
#include <Eigen/SparseCholesky>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace qucs_s::fem::detail {

namespace {

constexpr double Q = 1.602176634e-19;     // C
constexpr double KB = 1.380649e-23;       // J/K
constexpr double Hbar = 1.054571817e-34;  // J s
constexpr double M0 = 9.1093837015e-31;   // kg
constexpr double C0 = 299792458.0;        // m/s
constexpr double HB2M = 0.0380998212;     // ħ²/2m0 in eV nm²
constexpr int Carriers = 3;               // electrons, heavy holes, light holes
/// The band data kept with the field: its header's length.
constexpr int Header = 10;

/// A domain's bands (eV) and masses (m0) as solved.
struct Band {
    bool on = false;
    bool user = false;
    double Ec = 0, Ehh = 0, Elh = 0;   // strained, before the field's potential
    double me = 0, mhhZ = 0, mlhZ = 0;  // along the growth axis
    double mhhP = 0, mlhP = 0;          // across it
    double Ep = 0;                      // Kane's energy (eV)
    double nr = NaN;                    // refractive index
    double strain = 0;                  // in the plane
    Expression userEc, userEv;          // (as given: of x and y)
};

/// A point of integration: its domain, weight (nm, nm² or nm³ about an
/// axis), coordinate along the growth axis (m), each carrier's potential
/// (eV: Ec, -Ehh, -Elh), its element's equations and their shape
/// functions' values and gradients along and across the axis (1/nm).
struct Point {
    int d = -1;
    double w = 0, a = 0;
    double pot[Carriers] = {0, 0, 0};
    int n = 0;
    int eq[6];
    double N[6], ga[6], gc[6];
};

/// A state found: its energy (eV; a hole's below its band), its
/// wavefunction by equation (ψᵀ M ψ = 1), where it is along the axis (m),
/// whether it is bound (below the barriers' band edge), its mass across the
/// axis, Kane's energy and refractive index where it is.
struct State {
    double E = 0;
    Eigen::VectorXd x;
    double pos = 0;
    bool bound = false;
    double mPar = 0, Ep = 0, nr = 0;
};

const char* const CarrierName[Carriers] = {"e", "hh", "lh"};

double Inf()
{
    return std::numeric_limits<double>::infinity();
}

double fermi(double x)
{
    if (x > 40) return std::exp(-x);
    if (x < -40) return 1;
    return 1 / (1 + std::exp(x));
}

/// ln(1 + e^x), safe both ways.
double softplus(double x)
{
    return x > 30 ? x + std::log1p(std::exp(-x)) : std::log1p(std::exp(x));
}

class QuantumProblem : public Problem
{
public:
    QuantumProblem(Solution& solution, int field) : Problem(solution, field) {}

    bool setup(QString* error) override;
    void initialize() override;
    bool solve(const SolveOptions& options, QString* error) override;
    void setGlobals() override;
    int solveModes(const SolveOptions& options, int count, double guess, QString* error) override;
    void selectMode(int k) override;
    int solveSpectrum(const SpectrumOptions& o, QString* error) override;
    void selectSpectrum(int k) override;

private:
    /// 1D: the layers the line crosses, its elements and nodes.
    bool buildLine(QString* error);
    /// The points of integration with their potentials; the band data kept
    /// with the field.
    bool prepare(QString* error);
    /// The bands at the evaluator's point of domain \a d, \a a along the
    /// axis (m): Ec, Ehh, Elh (eV), the field's and the potential's in.
    void bandsAt(PointEvaluator& ev, int d, double a, double out[Carriers]) const;
    /// Carrier \a c's \a count lowest states.
    bool states(int c, int count, QString* error);
    /// ∫ ψa ψb f(domain, along the axis) over the points.
    double integral(const Eigen::VectorXd& a, const Eigen::VectorXd& b, const std::function<double(int, double)>& f) const;
    double mass(int c, int d, bool along) const;
    /// |ψ|² in SI (1/m, 1/m², 1/m³) from the integration's units.
    double densityScale() const;

    bool a_1d = true;
    int a_axis = 1;   // 0 x, 1 y
    bool a_on[Carriers] = {true, true, true};
    int a_order = 2;
    double a_T = 300, a_F = 0, a_s0 = 0, a_drawn = 0.05, a_h1d = 1e-10;
    Expression a_V;
    std::vector<Band> a_bands;   // by domain
    std::vector<int> a_flux;     // boundaries whose wavefunction is free
    // 1D: the line (m) and its nodes (m from its start), its elements' domains (-1 a gap).
    QPointF a_S, a_E;
    double a_L = 0;
    std::vector<double> a_z;
    std::vector<int> a_elemDomain;
    // The equations by degree of freedom (2D) or node (1D): -1 none.
    std::vector<int> a_eq;
    int a_neq = 0;
    std::vector<Point> a_points;
    std::vector<State> a_states[Carriers];
    double a_barrier[Carriers] = {NaN, NaN, NaN};   // Ec's least, Ehh's and Elh's greatest where the physics ends
    std::vector<char> a_active;                     // by domain: of the smallest gap (the wells)
    double a_activeSize = 0;                        // their length (m) or area (m²)
    int a_selected = -1;
    /// The first states' overlaps |<e_i|h_j>|² (by hole carrier), and the
    /// direct transition: the lowest of the electron and heavy hole states
    /// whose overlap is at least half the largest - a well's own e1-hh1, where
    /// a field puts the stack's lowest and highest states in different wells.
    std::vector<std::vector<double>> a_overlap[Carriers];
    double a_directE = NaN, a_directO = NaN;
    struct Spectrum {
        std::vector<double> E, te, tm;   // J, 1/m
        double Fc = NaN, Fv = NaN, size = 0, nr = 0;
        bool carriers = false;
    };
    Spectrum a_spec;
    int a_specSelected = -1;
};

double QuantumProblem::mass(int c, int d, bool along) const
{
    const Band& b = a_bands[std::size_t(d)];
    if (c == 0) return b.me;
    if (c == 1) return along ? b.mhhZ : b.mhhP;
    return along ? b.mlhZ : b.mlhP;
}

double QuantumProblem::densityScale() const
{
    if (a_1d) return 1e9;
    if (a_s.axisymmetric()) return 1e27 / (2 * M_PI);
    return 1e18;
}

bool QuantumProblem::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field& f = field();
    const Model& model = a_s.model();
    const Topology& topo = a_s.topology();
    const Scope& constants = a_s.parameters().scope;
    QString why;
    const QVector<int> domains = resolveSelection(topo, model, Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(topo.domains.size(), 0);
    for (int d : domains) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    a_1d = a_node->text(QStringLiteral("space")) != QLatin1String("2d");
    a_axis = a_node->text(QStringLiteral("axis")) == QLatin1String("x") ? 0 : 1;
    a_order = f.space->order();
    a_on[0] = a_node->flag(QStringLiteral("electrons"));
    a_on[1] = a_node->flag(QStringLiteral("heavy"));
    a_on[2] = a_node->flag(QStringLiteral("light"));
    if (!a_on[0] && !a_on[1] && !a_on[2]) problem(tr("%1 solves no carrier").arg(a_node->name()));
    if (a_1d && a_s.axisymmetric()) problem(tr("%1: about an axis it is 2D (dots: their states round the axis)").arg(a_node->name()));
    auto number = [&](const QString& text, const Dim& dim, const QString& what) {
        const std::optional<double> v = evaluateConstant(text, constants, dim, &why);
        if (!v) {
            problem(tr("%1: %2: %3").arg(a_node->name(), what, why));
            return NaN;
        }
        return *v;
    };
    // An energy in eV: a plain number or a voltage as eV, joules as such.
    auto energy = [&](const QString& text, const QString& what) {
        const Expression e = Expression::compile(text, constants);
        if (!e.isValid() || !e.isConstant()) {
            problem(tr("%1: %2: %3").arg(a_node->name(), what, e.isValid() ? tr("it must be a constant") : e.error()));
            return NaN;
        }
        return e.dim() == Dim::of(2, 1, -2) ? e.constant() / Q : e.constant();
    };
    a_T = number(a_node->text(QStringLiteral("T")), Dim::temperature(), tr("its temperature"));
    if (!(a_T > 0)) problem(tr("%1: its temperature must be more than 0").arg(a_node->name()));
    a_F = number(a_node->text(QStringLiteral("F")), Dim::of(1, 1, -3, -1), tr("its field"));
    if (!std::isfinite(a_F)) a_F = 0;
    a_drawn = energy(a_node->text(QStringLiteral("drawn")), tr("the height its waves are drawn"));
    if (!std::isfinite(a_drawn)) a_drawn = 0.05;
    if (a_1d) {
        a_h1d = number(a_node->text(QStringLiteral("h1d")), Dim::length(), tr("its element size"));
        if (!(a_h1d > 0)) {
            problem(tr("%1: its element size along the line must be more than 0").arg(a_node->name()));
            a_h1d = 1e-10;
        }
    }
    const QString vText = a_node->text(QStringLiteral("V")).trimmed();
    a_V = vText.isEmpty() || vText == QLatin1String("0") ? Expression() : compile(vText, *a_node, QStringLiteral("V"));
    // The substrate's lattice constant: a library material's, or a length.
    double substrate = NaN;
    const QString sub = a_node->text(QStringLiteral("substrate")).trimmed();
    if (!sub.isEmpty()) {
        QString compound;
        double sx = 0, sy = 0;
        for (const Node& m : model.materials().children)
            if (m.name().compare(sub, Qt::CaseInsensitive) == 0 || m.text(QStringLiteral("library")).compare(sub, Qt::CaseInsensitive) == 0) {
                compound = materialProperty(m, QStringLiteral("compound"));
                sx = evaluateConstant(materialProperty(m, QStringLiteral("x")), constants, Dim()).value_or(0);
                sy = evaluateConstant(materialProperty(m, QStringLiteral("y")), constants, Dim()).value_or(0);
                const QString a0 = materialProperty(m, QStringLiteral("a0"), &constants);
                if (!a0.trimmed().isEmpty() && compound.isEmpty()) substrate = number(a0, Dim::length(), tr("its substrate's lattice constant"));
            }
        if (compound.isEmpty() && !std::isfinite(substrate))
            if (const std::optional<MaterialEntry> e = findMaterial(sub)) {
                compound = e->properties.value(QStringLiteral("compound")).toString();
                sx = evaluateConstant(e->properties.value(QStringLiteral("x")).toString(), constants, Dim()).value_or(0);
                sy = evaluateConstant(e->properties.value(QStringLiteral("y")).toString(), constants, Dim()).value_or(0);
            }
        if (compound.isEmpty() && !std::isfinite(substrate)) {
            // A compound's name (GaAs) or a length.
            if (bandParameters(sub, 0, 0, a_T)) compound = sub;
            else substrate = number(sub, Dim::length(), tr("its substrate (a material or a lattice constant)"));
        }
        if (!compound.isEmpty()) {
            const std::optional<BandParameters> p = bandParameters(compound, sx, sy, a_T, &why);
            if (p) substrate = p->a0;
            else problem(tr("%1: its substrate: %2").arg(a_node->name(), why));
        }
        if (!(substrate > 0)) problem(tr("%1: its substrate's lattice constant must be more than 0").arg(a_node->name()));
    }
    // Each domain's bands and masses.
    const std::vector<const Node*> material = materials();
    a_bands.assign(topo.domains.size(), Band());
    std::vector<const Node*> modelOf(topo.domains.size(), nullptr);
    a_flux.clear();
    for (const Node& feature : a_node->children) {
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k) continue;
        const QVector<int> picked = resolveSelection(topo, model, k->selection, feature.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(feature.name(), why));
        if (feature.type == QLatin1String("quantum/material"))
            for (int d : picked) modelOf[std::size_t(d)] = &feature;
        else if (feature.type == QLatin1String("quantum/flux"))
            for (int b : picked) a_flux.push_back(b);
    }
    for (int d : domains) {
        Band& b = a_bands[std::size_t(d)];
        const Node* mf = modelOf[std::size_t(d)];
        const Node* mat = material[std::size_t(d)];
        const QString where = tr("domain %1 (%2)").arg(d + 1).arg(topo.objectNames.value(topo.domains[std::size_t(d)].owner));
        b.on = true;
        if (mf && mf->text(QStringLiteral("source")) == QLatin1String("user")) {
            b.user = true;
            b.userEc = compile(mf->text(QStringLiteral("Ec")), *mf, QStringLiteral("Ec"));
            b.userEv = compile(mf->text(QStringLiteral("Ev")), *mf, QStringLiteral("Ev"));
            b.me = number(mf->text(QStringLiteral("me")), Dim(), tr("%1's electron mass").arg(where));
            b.mhhZ = number(mf->text(QStringLiteral("mhh")), Dim(), tr("%1's heavy hole mass").arg(where));
            b.mlhZ = number(mf->text(QStringLiteral("mlh")), Dim(), tr("%1's light hole mass").arg(where));
            const QString hp = mf->text(QStringLiteral("mhhp")).trimmed(), lp = mf->text(QStringLiteral("mlhp")).trimmed();
            b.mhhP = hp.isEmpty() ? b.mhhZ : number(hp, Dim(), where);
            b.mlhP = lp.isEmpty() ? b.mlhZ : number(lp, Dim(), where);
            b.Ep = energy(mf->text(QStringLiteral("Ep")), tr("%1's Kane energy").arg(where));
            b.nr = number(mf->text(QStringLiteral("nr")), Dim(), tr("%1's refractive index").arg(where));
        } else {
            if (!mat) {
                problem(tr("%1 has no material").arg(where));
                continue;
            }
            auto text = [&](const QString& key) { return materialProperty(*mat, key, &constants); };
            const QString compound = materialProperty(*mat, QStringLiteral("compound")).trimmed();
            std::optional<BandParameters> p;
            if (!compound.isEmpty()) {
                const double x = evaluateConstant(materialProperty(*mat, QStringLiteral("x")), constants, Dim()).value_or(0);
                const double y = evaluateConstant(materialProperty(*mat, QStringLiteral("y")), constants, Dim()).value_or(0);
                p = bandParameters(compound, x, y, a_T, &why);
                if (!p) {
                    problem(tr("%1: %2").arg(where, why));
                    continue;
                }
            }
            // A property given on the material over its compound's.
            auto value = [&](const QString& key, double fromCompound, const Dim& dim, bool eV) {
                const QString own = mat->text(key).trimmed();
                if (!own.isEmpty() || !p) {
                    const QString t = own.isEmpty() ? text(key) : own;
                    if (t.trimmed().isEmpty()) return NaN;
                    return eV ? energy(t, tr("%1's %2").arg(where, key)) : number(t, dim, tr("%1's %2").arg(where, key));
                }
                return fromCompound;
            };
            const double Eg = value(QStringLiteral("Eg"), p ? p->Eg : NaN, Dim(), true);
            double VBO = value(QStringLiteral("VBO"), p ? p->VBO : NaN, Dim(), true);
            if (!std::isfinite(VBO)) {
                // The affinity's rule: Ev = -χ - Eg.
                const QString chi = text(QStringLiteral("chi"));
                if (!chi.trimmed().isEmpty() && std::isfinite(Eg)) VBO = -energy(chi, where) - Eg;
            }
            if (!std::isfinite(Eg) || !std::isfinite(VBO)) {
                problem(tr("%1: its material has no band gap and valence band offset (Eg with VBO or χ): give its compound, or "
                           "its bands as a Quantum Well Material of its own")
                            .arg(where));
                continue;
            }
            b.me = value(QStringLiteral("me"), p ? p->me : NaN, Dim(), false);
            const double g1 = value(QStringLiteral("gamma1"), p ? p->gamma1 : NaN, Dim(), false);
            const double g2 = value(QStringLiteral("gamma2"), p ? p->gamma2 : NaN, Dim(), false);
            if (!std::isfinite(b.me) || !std::isfinite(g1) || !std::isfinite(g2) || !(g1 > 2 * g2)) {
                problem(tr("%1: its material has no effective masses (me, and Luttinger's γ1 > 2 γ2)").arg(where));
                continue;
            }
            b.mhhZ = 1 / (g1 - 2 * g2);
            b.mlhZ = 1 / (g1 + 2 * g2);
            b.mhhP = 1 / (g1 + g2);
            b.mlhP = 1 / (g1 - g2);
            b.Ep = value(QStringLiteral("Ep"), p ? p->Ep : NaN, Dim(), true);
            if (!std::isfinite(b.Ep)) b.Ep = 20;
            const QString nText = text(QStringLiteral("n"));
            b.nr = nText.trimmed().isEmpty() ? (p ? p->n : NaN) : evaluateConstant(nText, constants, Dim()).value_or(p ? p->n : NaN);
            b.Ec = VBO + Eg;
            b.Ehh = b.Elh = VBO;
            if (std::isfinite(substrate)) {
                // Pseudomorphic on the substrate: biaxial strain in the plane
                // (Bir and Pikus; Chuang's signs).
                const double a0 = value(QStringLiteral("a0"), p ? p->a0 : NaN, Dim::length(), false);
                const double ac = value(QStringLiteral("ac"), p ? p->ac : NaN, Dim(), true);
                const double av = value(QStringLiteral("av"), p ? p->av : NaN, Dim(), true);
                const double bs = value(QStringLiteral("b"), p ? p->b : NaN, Dim(), true);
                const double c11 = value(QStringLiteral("C11"), p ? p->C11 : NaN, Dim::of(-1, 1, -2), false);
                const double c12 = value(QStringLiteral("C12"), p ? p->C12 : NaN, Dim::of(-1, 1, -2), false);
                const double delta = value(QStringLiteral("Delta"), p ? p->Delta : NaN, Dim(), true);
                if (!std::isfinite(a0) || !std::isfinite(ac) || !std::isfinite(av) || !std::isfinite(bs) || !(c11 > 0) || !std::isfinite(c12)) {
                    problem(tr("%1: its material has not what strain needs (a0, ac, av, b, C11, C12)").arg(where));
                    continue;
                }
                const double exx = (substrate - a0) / a0, ezz = -2 * c12 / c11 * exx;
                const double vol = 2 * exx + ezz;
                const double P = -av * vol, Qe = -bs * (exx - ezz);
                const double D = std::isfinite(delta) ? delta : 0.3;
                b.strain = exx;
                b.Ec += ac * vol;
                b.Ehh = VBO - P - Qe;
                b.Elh = VBO - P + 0.5 * (Qe - D + std::sqrt(D * D + 2 * D * Qe + 9 * Qe * Qe));
            }
        }
        if (!(b.me > 0) || !(b.mhhZ > 0) || !(b.mlhZ > 0) || !(b.mhhP > 0) || !(b.mlhP > 0))
            problem(tr("%1: its effective masses must be more than 0").arg(where));
    }
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    return true;
}

void QuantumProblem::initialize()
{
    Field& f = field();
    f.values.assign(std::size_t(f.space->size()) * std::size_t(f.count()), 0.0);
    f.extra.clear();
}

bool QuantumProblem::buildLine(QString* error)
{
    const Field& f = field();
    const Topology& topo = a_s.topology();
    const Mesh& mesh = a_s.mesh();
    const double unit = mesh.unitScale;
    QRectF box;
    for (std::size_t d = 0; d < topo.domains.size(); ++d)
        if (f.domains[d]) box = box.isNull() ? topo.domains[d].bounds : box.united(topo.domains[d].bounds);
    double at = (a_axis == 1 ? box.center().x() : box.center().y()) * unit;
    const QString atText = a_node->text(QStringLiteral("at")).trimmed();
    if (!atText.isEmpty()) {
        Expression::Options inUnit;
        inUnit.lengthUnit = unit;
        inUnit.numbersAreLengths = true;
        const Expression e = Expression::compile(atText, a_s.parameters().scope, inUnit);
        if (!e.isValid() || !e.isConstant()) {
            *error = tr("%1: where its line runs: %2").arg(a_node->name(), e.isValid() ? tr("it must be a constant") : e.error());
            return false;
        }
        at = e.constant() * unit;
    }
    if (a_axis == 1) {
        a_S = QPointF(at, box.top() * unit);
        a_E = QPointF(at, box.bottom() * unit);
    } else {
        a_S = QPointF(box.left() * unit, at);
        a_E = QPointF(box.right() * unit, at);
    }
    a_L = std::hypot(a_E.x() - a_S.x(), a_E.y() - a_S.y());
    if (!(a_L > 0)) {
        *error = tr("%1: its domains have no extent along the axis").arg(a_node->name());
        return false;
    }
    const QPointF u = (a_E - a_S) / a_L;
    // Each triangle's stretch of the line (its corners counter-clockwise).
    struct Piece {
        double lo, hi;
        int d;
    };
    std::vector<Piece> pieces;
    std::vector<double> cuts = {0, a_L};
    const double eps = 1e-9 * a_L;
    for (std::size_t t = 0; t < mesh.triangles.size(); ++t) {
        QPointF p[3];
        for (int k = 0; k < 3; ++k) p[k] = mesh.nodes[std::size_t(mesh.triangles[t][std::size_t(k)])] * unit;
        double lo = 0, hi = a_L;
        bool empty = false;
        for (int k = 0; k < 3 && !empty; ++k) {
            const QPointF a = p[k], e = p[(k + 1) % 3] - p[k];
            const double alpha = e.x() * (a_S.y() - a.y()) - e.y() * (a_S.x() - a.x());
            const double beta = e.x() * u.y() - e.y() * u.x();
            const double scale = std::hypot(e.x(), e.y());
            if (std::abs(beta) < 1e-12) {
                if (alpha < -1e-12 * scale * a_L) empty = true;
            } else if (beta > 0) lo = std::max(lo, -alpha / beta);
            else hi = std::min(hi, -alpha / beta);
        }
        if (empty || hi - lo <= eps) continue;
        pieces.push_back({lo, hi, mesh.domain[t]});
        cuts.push_back(lo);
        cuts.push_back(hi);
    }
    std::sort(cuts.begin(), cuts.end());
    std::vector<double> breaks;
    for (double c : cuts)
        if (breaks.empty() || c - breaks.back() > eps) breaks.push_back(c);
    // The layers: each stretch between two cuts in the domain at its middle.
    struct Layer {
        double s0, s1;
        int d;
    };
    std::vector<Layer> layers;
    for (std::size_t k = 0; k + 1 < breaks.size(); ++k) {
        const double m = (breaks[k] + breaks[k + 1]) / 2;
        int d = -1;
        for (const Piece& p : pieces)
            if (m > p.lo && m < p.hi) {
                d = p.d;
                break;
            }
        if (d >= 0 && !f.domains[std::size_t(d)]) d = -1;
        if (!layers.empty() && layers.back().d == d) layers.back().s1 = breaks[k + 1];
        else layers.push_back({breaks[k], breaks[k + 1], d});
    }
    // Quadratic elements: as many as the size asks in each layer, one across a gap.
    a_z.assign(1, 0.0);
    a_elemDomain.clear();
    long total = 0;
    for (const Layer& l : layers) total += l.d >= 0 ? long(std::ceil((l.s1 - l.s0) / a_h1d)) : 1;
    if (total > 400000) {
        *error = tr("%1: %2 elements along its line: a larger element size").arg(a_node->name()).arg(total);
        return false;
    }
    for (const Layer& l : layers) {
        const int n = l.d >= 0 ? std::max(1, int(std::ceil((l.s1 - l.s0) / a_h1d - 1e-9))) : 1;
        for (int k = 0; k < n; ++k) {
            const double z0 = l.s0 + (l.s1 - l.s0) * k / n, z1 = l.s0 + (l.s1 - l.s0) * (k + 1) / n;
            a_z.push_back((z0 + z1) / 2);
            a_z.push_back(z1);
            a_elemDomain.push_back(l.d);
        }
    }
    // A node is free where it is inside the physics' run of elements.
    const int nn = int(a_z.size()), ne = int(a_elemDomain.size());
    std::vector<int> touching(std::size_t(nn), 0);
    for (int e = 0; e < ne; ++e)
        if (a_elemDomain[std::size_t(e)] >= 0) {
            ++touching[std::size_t(2 * e)];
            touching[std::size_t(2 * e + 1)] += 2;
            ++touching[std::size_t(2 * e + 2)];
        }
    a_eq.assign(std::size_t(nn), -1);
    a_neq = 0;
    for (int i = 0; i < nn; ++i)
        if (touching[std::size_t(i)] >= 2) a_eq[std::size_t(i)] = a_neq++;
    if (a_neq == 0) {
        *error = tr("%1: its line at %2 crosses none of its domains").arg(a_node->name(), formatQuantity(at, Dim::length()));
        return false;
    }
    return true;
}

void QuantumProblem::bandsAt(PointEvaluator& ev, int d, double a, double out[Carriers]) const
{
    const Band& b = a_bands[std::size_t(d)];
    double ec = b.Ec, ehh = b.Ehh, elh = b.Elh;
    if (b.user) {
        auto eV = [&](const Expression& e) {
            const double v = ev.eval(e);
            return e.dim() == Dim::of(2, 1, -2) ? v / Q : v;
        };
        ec = eV(b.userEc);
        ehh = elh = eV(b.userEv);
    }
    double U = a_F * (a - a_s0);
    if (a_V.isValid()) {
        const double v = ev.eval(a_V);
        if (std::isfinite(v)) U -= v;
    }
    out[0] = ec + U;
    out[1] = ehh + U;
    out[2] = elh + U;
}

bool QuantumProblem::prepare(QString* error)
{
    Field& f = field();
    const Mesh& mesh = a_s.mesh();
    const Topology& topo = a_s.topology();
    const Space& sp = *f.space;
    PointEvaluator ev(a_s);
    a_points.clear();
    // The middle of the physics along the axis: the field's potential 0 there.
    {
        double lo = Inf(), hi = -Inf();
        for (std::size_t d = 0; d < topo.domains.size(); ++d)
            if (f.domains[d]) {
                const QRectF& r = topo.domains[d].bounds;
                lo = std::min(lo, (a_axis == 1 ? r.top() : r.left()) * mesh.unitScale);
                hi = std::max(hi, (a_axis == 1 ? r.bottom() : r.right()) * mesh.unitScale);
            }
        a_s0 = (lo + hi) / 2;
    }
    auto axisOf = [&](QPointF p) { return a_axis == 1 ? p.y() : p.x(); };
    if (a_1d) {
        if (!buildLine(error)) return false;
        const Locator locator(mesh);
        const QPointF u = (a_E - a_S) / a_L;
        const auto& rule = lineRule(4);
        for (std::size_t e = 0; e < a_elemDomain.size(); ++e) {
            const int d = a_elemDomain[e];
            if (d < 0) continue;
            const double z0 = a_z[2 * e], z2 = a_z[2 * e + 2], h = z2 - z0, hnm = h * 1e9;
            for (const auto& q : rule) {
                const double xi = q[0];
                Point pt;
                pt.d = d;
                pt.w = q[1] * hnm;
                pt.n = 3;
                const double N[3] = {(1 - xi) * (1 - 2 * xi), 4 * xi * (1 - xi), xi * (2 * xi - 1)};
                const double dN[3] = {4 * xi - 3, 4 - 8 * xi, 4 * xi - 1};
                for (int i = 0; i < 3; ++i) {
                    pt.eq[i] = a_eq[2 * e + std::size_t(i)];
                    pt.N[i] = N[i];
                    pt.ga[i] = dN[i] / hnm;
                    pt.gc[i] = 0;
                }
                const QPointF P = a_S + u * (z0 + xi * h);
                pt.a = axisOf(P);
                // (On a mesh edge it may be in neither triangle by a rounding:
                // nudged across the line.)
                int t = -1;
                double rx = 0, ry = 0;
                bool located = false;
                const QPointF across(-u.y() * 1e-6 * a_L, u.x() * 1e-6 * a_L);
                for (const QPointF& p : {P, P + across, P - across})
                    if (!located && locator.locate(p / mesh.unitScale, &t, &rx, &ry)) located = true;
                double b[Carriers];
                if (located) {
                    ev.moveTo(t, rx, ry);
                    bandsAt(ev, d, pt.a, b);
                } else {
                    // Nowhere to evaluate an expression: the bands' constants and the field.
                    const Band& band = a_bands[std::size_t(d)];
                    const double U = a_F * (pt.a - a_s0);
                    b[0] = band.user ? NaN : band.Ec + U;
                    b[1] = band.user ? NaN : band.Ehh + U;
                    b[2] = band.user ? NaN : band.Elh + U;
                }
                pt.pot[0] = b[0];
                pt.pot[1] = -b[1];
                pt.pot[2] = -b[2];
                a_points.push_back(pt);
            }
        }
    } else {
        // The degrees of freedom of its triangles; those where the physics
        // ends (but where its wavefunction is free) fixed at 0.
        const int nd = sp.size();
        std::vector<char> on(std::size_t(nd), 0), fixed(std::size_t(nd), 0);
        for (int t = 0; t < sp.elements(); ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            for (int i = 0; i < sp.perElement(); ++i) on[std::size_t(sp.dofs(t)[i])] = 1;
        }
        auto inside = [&](int t) { return t >= 0 && mesh.domain[std::size_t(t)] >= 0 && f.domains[std::size_t(mesh.domain[std::size_t(t)])]; };
        for (std::size_t e = 0; e < mesh.edges.size(); ++e) {
            const Mesh::Edge& me = mesh.edges[e];
            if (inside(me.left) == inside(me.right)) continue;
            if (std::find(a_flux.begin(), a_flux.end(), me.boundary) != a_flux.end()) continue;
            for (int dof : sp.edgeDofs(int(e)))
                if (dof >= 0) fixed[std::size_t(dof)] = 1;
        }
        a_eq.assign(std::size_t(nd), -1);
        a_neq = 0;
        for (int i = 0; i < nd; ++i)
            if (on[std::size_t(i)] && !fixed[std::size_t(i)]) a_eq[std::size_t(i)] = a_neq++;
        if (a_neq == 0) {
            *error = tr("%1: no degree of freedom is free").arg(a_node->name());
            return false;
        }
        const TriangleRule& rule = triangleRule(a_order == 1 ? 3 : 5);
        const bool axi = a_s.axisymmetric();
        for (int t = 0; t < sp.elements(); ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            for (const auto& q : rule.points) {
                const Space::Map m = sp.map(t, q[0], q[1]);
                Shape sh;
                shapeAt(sp.order(), q[0], q[1], sh);
                Point pt;
                pt.d = d;
                pt.w = q[2] * std::abs(m.det) * 1e18 * (axi ? m.x.x() * 1e9 : 1.0);
                pt.a = axisOf(m.x);
                pt.n = sh.count;
                for (int i = 0; i < sh.count; ++i) {
                    double gx = 0, gy = 0;
                    Space::gradient(m, sh.dxi[i], sh.deta[i], gx, gy);
                    pt.eq[i] = a_eq[std::size_t(sp.dofs(t)[i])];
                    pt.N[i] = sh.n[i];
                    pt.ga[i] = (a_axis == 1 ? gy : gx) * 1e-9;
                    pt.gc[i] = (a_axis == 1 ? gx : gy) * 1e-9;
                }
                ev.moveTo(t, q[0], q[1]);
                double b[Carriers];
                bandsAt(ev, d, pt.a, b);
                pt.pot[0] = b[0];
                pt.pot[1] = -b[1];
                pt.pot[2] = -b[2];
                a_points.push_back(pt);
            }
        }
    }
    // The bands kept with the field, for its variables: at each triangle's
    // corners (each its own: the offsets stay sharp), the masses by domain.
    const int nt = int(mesh.triangles.size()), ndom = int(topo.domains.size());
    auto fixedData = std::make_shared<std::vector<double>>();
    std::vector<double>& fx = *fixedData;
    fx.assign(std::size_t(Header) + a_z.size() + std::size_t(9 * nt + 4 * ndom), NaN);
    fx[0] = a_1d ? 1 : 2;
    fx[1] = a_axis;
    fx[2] = a_S.x();
    fx[3] = a_S.y();
    fx[4] = a_E.x();
    fx[5] = a_E.y();
    fx[6] = double(a_z.size());
    fx[7] = a_drawn * Q;
    fx[8] = nt;
    fx[9] = ndom;
    std::copy(a_z.begin(), a_z.end(), fx.begin() + Header);
    const std::size_t triBase = std::size_t(Header) + a_z.size(), domBase = triBase + std::size_t(9 * nt);
    static const double corner[3][2] = {{0, 0}, {1, 0}, {0, 1}};
    for (int t = 0; t < nt; ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)] || !a_bands[std::size_t(d)].on) continue;
        for (int k = 0; k < 3; ++k) {
            ev.moveTo(t, corner[k][0], corner[k][1]);
            double b[Carriers];
            bandsAt(ev, d, axisOf(ev.point()), b);
            for (int c = 0; c < Carriers; ++c) fx[triBase + std::size_t(9 * t + 3 * c + k)] = b[c] * Q;
        }
    }
    for (int d = 0; d < ndom; ++d) {
        const Band& b = a_bands[std::size_t(d)];
        if (!b.on) continue;
        fx[domBase + std::size_t(4 * d)] = b.me;
        fx[domBase + std::size_t(4 * d + 1)] = b.mhhZ;
        fx[domBase + std::size_t(4 * d + 2)] = b.mlhZ;
        fx[domBase + std::size_t(4 * d + 3)] = b.strain;
    }
    f.fixed = fixedData;
    // Where the physics ends: the band edges past which a state is not bound.
    for (int c = 0; c < Carriers; ++c) a_barrier[c] = c == 0 ? Inf() : -Inf();
    if (a_1d) {
        // The first and last points of each run of elements.
        const auto& rule = lineRule(4);
        const std::size_t per = rule.size();
        std::size_t p = 0;
        for (std::size_t e = 0; e < a_elemDomain.size(); ++e) {
            if (a_elemDomain[e] < 0) continue;
            const bool startsRun = e == 0 || a_elemDomain[e - 1] < 0;
            const bool endsRun = e + 1 == a_elemDomain.size() || a_elemDomain[e + 1] < 0;
            for (const std::size_t k : {std::size_t(0), per - 1}) {
                if ((k == 0 && !startsRun) || (k == per - 1 && !endsRun)) continue;
                const Point& pt = a_points[p + k];
                a_barrier[0] = std::min(a_barrier[0], pt.pot[0]);
                a_barrier[1] = std::max(a_barrier[1], -pt.pot[1]);
                a_barrier[2] = std::max(a_barrier[2], -pt.pot[2]);
            }
            p += per;
        }
    } else {
        auto inside = [&](int t) { return t >= 0 && mesh.domain[std::size_t(t)] >= 0 && f.domains[std::size_t(mesh.domain[std::size_t(t)])]; };
        for (const Mesh::Edge& me : mesh.edges) {
            if (inside(me.left) == inside(me.right)) continue;
            const int t = inside(me.left) ? me.left : me.right;
            const auto& tri = mesh.triangles[std::size_t(t)];
            for (int k = 0; k < 3; ++k) {
                if (tri[std::size_t(k)] != me.a && tri[std::size_t(k)] != me.b) continue;
                const double ec = fx[triBase + std::size_t(9 * t + k)] / Q;
                const double eh = fx[triBase + std::size_t(9 * t + 3 + k)] / Q, el = fx[triBase + std::size_t(9 * t + 6 + k)] / Q;
                a_barrier[0] = std::min(a_barrier[0], ec);
                a_barrier[1] = std::max(a_barrier[1], eh);
                a_barrier[2] = std::max(a_barrier[2], el);
            }
        }
    }
    // The wells: the domains of the smallest gap.
    std::vector<double> gapSum(std::size_t(ndom), 0), wSum(std::size_t(ndom), 0);
    for (const Point& pt : a_points) {
        gapSum[std::size_t(pt.d)] += pt.w * (pt.pot[0] + std::min(pt.pot[1], pt.pot[2]));
        wSum[std::size_t(pt.d)] += pt.w;
    }
    double least = Inf();
    for (int d = 0; d < ndom; ++d)
        if (wSum[std::size_t(d)] > 0) least = std::min(least, gapSum[std::size_t(d)] / wSum[std::size_t(d)]);
    a_active.assign(std::size_t(ndom), 0);
    double size = 0;
    for (int d = 0; d < ndom; ++d)
        if (wSum[std::size_t(d)] > 0 && gapSum[std::size_t(d)] / wSum[std::size_t(d)] < least + 1e-3) {
            a_active[std::size_t(d)] = 1;
            size += wSum[std::size_t(d)];
        }
    a_activeSize = size * (a_1d ? 1e-9 : 1e-18);
    return true;
}

double QuantumProblem::integral(const Eigen::VectorXd& a, const Eigen::VectorXd& b, const std::function<double(int, double)>& f) const
{
    double sum = 0;
    for (const Point& pt : a_points) {
        double va = 0, vb = 0;
        for (int i = 0; i < pt.n; ++i)
            if (pt.eq[i] >= 0) {
                va += pt.N[i] * a(pt.eq[i]);
                vb += pt.N[i] * b(pt.eq[i]);
            }
        sum += pt.w * va * vb * (f ? f(pt.d, pt.a) : 1.0);
    }
    return sum;
}

bool QuantumProblem::states(int c, int count, QString* error)
{
    a_states[c].clear();
    if (!a_on[c]) return true;
    const int n = a_neq;
    std::vector<Eigen::Triplet<double>> tk, tm;
    tk.reserve(a_points.size() * 36);
    tm.reserve(a_points.size() * 36);
    double lowest = Inf();
    for (const Point& pt : a_points) {
        const double along = HB2M / mass(c, pt.d, true), across = HB2M / mass(c, pt.d, false);
        lowest = std::min(lowest, pt.pot[c]);
        for (int i = 0; i < pt.n; ++i) {
            if (pt.eq[i] < 0) continue;
            for (int j = 0; j < pt.n; ++j) {
                if (pt.eq[j] < 0) continue;
                const double m = pt.w * pt.N[i] * pt.N[j];
                tk.emplace_back(pt.eq[i], pt.eq[j], pt.w * (along * pt.ga[i] * pt.ga[j] + across * pt.gc[i] * pt.gc[j]) + pt.pot[c] * m);
                tm.emplace_back(pt.eq[i], pt.eq[j], m);
            }
        }
    }
    Eigen::SparseMatrix<double> K(n, n), M(n, n);
    K.setFromTriplets(tk.begin(), tk.end());
    M.setFromTriplets(tm.begin(), tm.end());
    if (!std::isfinite(lowest)) {
        *error = tr("%1: its bands are not finite (a potential not defined where it is solved?)").arg(a_node->name());
        return false;
    }
    count = std::min(count, n);
    std::vector<std::pair<double, Eigen::VectorXd>> found;
    if (n <= 600) {
        const Eigen::MatrixXd Kd(K), Md(M);
        Eigen::GeneralizedSelfAdjointEigenSolver<Eigen::MatrixXd> es(Kd, Md);
        if (es.info() != Eigen::Success) {
            *error = tr("%1: the %2 states' eigenproblem was not solved").arg(a_node->name(), QLatin1String(CarrierName[c]));
            return false;
        }
        for (int k = 0; k < count; ++k) found.emplace_back(es.eigenvalues()(k), es.eigenvectors().col(k));
    } else {
        // Shift and invert below the lowest potential: (K - σM) is positive
        // definite, its inverse's largest eigenvalues the lowest states.
        const double sigma = lowest - 0.01 - 1e-3 * std::abs(lowest);
        Eigen::SparseMatrix<double> A = K - sigma * M;
        A.makeCompressed();
        Eigen::SimplicialLLT<Eigen::SparseMatrix<double>> llt(A);
        if (llt.info() != Eigen::Success) {
            *error = tr("%1: the shifted %2 matrix is not positive definite").arg(a_node->name(), QLatin1String(CarrierName[c]));
            return false;
        }
        const int asked = std::min(count + 3, n - 2);
        const EigenResult r = largestEigenvalues(n, asked, [&](const Eigen::VectorXcd& x, Eigen::VectorXcd& y) {
            const Eigen::VectorXd re = M * x.real(), im = M * x.imag();
            const Eigen::VectorXd sr = llt.solve(re), si = llt.solve(im);
            y.resize(n);
            for (int i = 0; i < n; ++i) y(i) = std::complex<double>(sr(i), si(i));
        });
        if (!r.error.isEmpty()) a_s.log << tr("Warning: %1: %2").arg(a_node->name(), r.error);
        // Rayleigh and Ritz in the real space they span (a degenerate pair
        // comes out of the solver mixed, complex): M-orthonormal, then K's
        // eigenvectors there.
        std::vector<Eigen::VectorXd> basis;
        auto addVector = [&](Eigen::VectorXd v) {
            for (int pass = 0; pass < 2; ++pass)
                for (const Eigen::VectorXd& b : basis) v -= b * b.dot(M * v);
            const double norm = std::sqrt(std::max(0.0, v.dot(M * v)));
            if (norm > 1e-8) basis.push_back(v / norm);
        };
        for (const Eigen::VectorXcd& v : r.vectors) {
            const double scale = v.norm();
            if (!(scale > 0)) continue;
            addVector(v.real() / scale);
            addVector(v.imag() / scale);
        }
        if (basis.empty()) {
            *error = tr("%1: no %2 state was found").arg(a_node->name(), QLatin1String(CarrierName[c]));
            return false;
        }
        const int m = int(basis.size());
        Eigen::MatrixXd B(n, m);
        for (int k = 0; k < m; ++k) B.col(k) = basis[std::size_t(k)];
        const Eigen::MatrixXd small = B.transpose() * (K * B);
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(0.5 * (small + small.transpose()));
        for (int k = 0; k < std::min(count, m); ++k) found.emplace_back(es.eigenvalues()(k), B * es.eigenvectors().col(k));
    }
    for (auto& [lambda, x] : found) {
        State s;
        // (Normalized: xᵀ M x = 1; its largest value positive.)
        const double norm = std::sqrt(std::max(1e-300, x.dot(M * x)));
        x /= norm;
        Eigen::Index big = 0;
        x.cwiseAbs().maxCoeff(&big);
        if (x(big) < 0) x = -x;
        s.E = c == 0 ? lambda : -lambda;
        s.x = x;
        s.pos = integral(x, x, [](int, double a) { return a; });
        s.bound = c == 0 ? s.E < a_barrier[0] : s.E > a_barrier[c];
        const double invM = integral(x, x, [&](int d, double) { return 1 / mass(c, d, false); });
        s.mPar = invM > 0 ? 1 / invM : NaN;
        s.Ep = integral(x, x, [&](int d, double) { return a_bands[std::size_t(d)].Ep; });
        s.nr = integral(x, x, [&](int d, double) {
            const double v = a_bands[std::size_t(d)].nr;
            return std::isfinite(v) ? v : 3.5;
        });
        a_states[c].push_back(std::move(s));
    }
    std::sort(a_states[c].begin(), a_states[c].end(), [c](const State& a, const State& b) { return c == 0 ? a.E < b.E : a.E > b.E; });
    return true;
}

int QuantumProblem::solveModes(const SolveOptions& options, int count, double guess, QString* error)
{
    (void)options;
    (void)guess;
    if (!prepare(error)) return -1;
    int most = 0;
    for (int c = 0; c < Carriers; ++c) {
        if (!states(c, std::max(1, count), error)) return -1;
        most = std::max(most, int(a_states[c].size()));
    }
    // The overlaps the globals give, and the direct transition.
    for (int c = 1; c < Carriers; ++c) {
        a_overlap[c].assign(std::min<std::size_t>(4, a_states[0].size()), {});
        for (std::size_t i = 0; i < a_overlap[c].size(); ++i)
            for (std::size_t j = 0; j < std::min<std::size_t>(4, a_states[c].size()); ++j) {
                const double ov = integral(a_states[0][i].x, a_states[c][j].x, {});
                a_overlap[c][i].push_back(ov * ov);
            }
    }
    a_directE = a_directO = NaN;
    {
        std::vector<std::array<double, 2>> pairs;   // energy (eV), overlap
        double best = 0;
        for (const State& e : a_states[0])
            for (const State& h : a_states[1]) {
                const double ov = integral(e.x, h.x, {});
                pairs.push_back({e.E - h.E, ov * ov});
                best = std::max(best, ov * ov);
            }
        for (const auto& p : pairs)
            if (best > 0 && p[1] >= best / 2 && !(p[0] >= a_directE)) {
                a_directE = p[0];
                a_directO = p[1];
            }
    }
    QStringList found;
    for (int c = 0; c < Carriers; ++c) {
        if (!a_on[c]) continue;
        int bound = 0;
        for (const State& s : a_states[c]) bound += s.bound ? 1 : 0;
        found << tr("%1 %2 (%3 bound)").arg(a_states[c].size()).arg(QLatin1String(CarrierName[c])).arg(bound);
    }
    a_s.log << tr("%1: %2 states, %3 degrees of freedom%4")
                   .arg(a_node->name(), found.join(QLatin1String(", ")))
                   .arg(a_neq)
                   .arg(a_1d ? tr(" along a line of %1").arg(formatQuantity(a_L, Dim::length())) : QString());
    field().solved = true;
    a_spec = Spectrum();
    a_specSelected = -1;
    return most;
}

void QuantumProblem::selectMode(int k)
{
    Field& f = field();
    a_selected = k;
    const Space& sp = *f.space;
    const int nd = sp.size();
    const double scale = std::sqrt(densityScale());
    f.values.assign(std::size_t(nd) * std::size_t(Carriers), 0.0);
    const std::size_t n1 = a_1d ? a_z.size() : 0;
    f.extra.assign(std::size_t(2 * Carriers) + Carriers * n1, NaN);
    // A 1D state's value at a node, and anywhere along the line (m).
    auto along = [&](const Eigen::VectorXd& x, std::size_t node) { return a_eq[node] >= 0 ? x(a_eq[node]) : 0.0; };
    for (int c = 0; c < Carriers; ++c) {
        if (k < 0 || k >= int(a_states[c].size())) continue;
        const State& s = a_states[c][std::size_t(k)];
        f.extra[std::size_t(c)] = s.E * Q;
        double peak = 0;
        if (a_1d) {
            for (std::size_t i = 0; i < n1; ++i) {
                const double v = along(s.x, i) * scale;
                f.extra[std::size_t(2 * Carriers) + std::size_t(c) * n1 + i] = v;
                peak = std::max(peak, v * v);
            }
            // The 2D degrees of freedom: the line's value where they project.
            const QPointF u = (a_E - a_S) / a_L;
            for (int dof = 0; dof < nd; ++dof) {
                const QPointF p = sp.point(dof) - a_S;
                const double z = p.x() * u.x() + p.y() * u.y();
                if (z < 0 || z > a_L) continue;
                const auto it = std::upper_bound(a_z.begin(), a_z.end(), z);
                std::size_t e = std::size_t(std::max<std::ptrdiff_t>(0, (it - a_z.begin()) - 1)) / 2;
                e = std::min(e, a_elemDomain.size() - 1);
                const double z0 = a_z[2 * e], z2 = a_z[2 * e + 2];
                const double xi = z2 > z0 ? (z - z0) / (z2 - z0) : 0;
                const double v = (1 - xi) * (1 - 2 * xi) * along(s.x, 2 * e) + 4 * xi * (1 - xi) * along(s.x, 2 * e + 1)
                                 + xi * (2 * xi - 1) * along(s.x, 2 * e + 2);
                f.values[std::size_t(c) * std::size_t(nd) + std::size_t(dof)] = v * scale;
            }
        } else {
            for (int dof = 0; dof < nd; ++dof) {
                const int eq = a_eq[std::size_t(dof)];
                if (eq < 0) continue;
                const double v = s.x(eq) * scale;
                f.values[std::size_t(c) * std::size_t(nd) + std::size_t(dof)] = v;
                peak = std::max(peak, v * v);
            }
        }
        f.extra[std::size_t(Carriers + c)] = peak;
    }
    a_s.setMode(k + 1);
}

bool QuantumProblem::solve(const SolveOptions& options, QString* error)
{
    // At rest: each carrier's four lowest states, the first selected.
    if (solveModes(options, 4, 0, error) < 0) return false;
    selectMode(0);
    return true;
}

int QuantumProblem::solveSpectrum(const SpectrumOptions& o, QString* error)
{
    if (solveModes(o.solve, std::max(1, o.states), 0, error) < 0) return -1;
    if (!a_on[0] || (!a_on[1] && !a_on[2])) {
        *error = tr("%1: a spectrum wants electrons and holes").arg(a_node->name());
        return -1;
    }
    if (a_s.axisymmetric()) {
        *error = tr("%1: a dot's spectrum (about an axis) is not found yet: 1D wells or 2D wires").arg(a_node->name());
        return -1;
    }
    if (!(a_activeSize > 0)) {
        *error = tr("%1: it has no wells").arg(a_node->name());
        return -1;
    }
    const bool wells = a_1d;   // 2D densities of states (free in the plane); a wire's 1D (free along it)
    const double kT = KB * a_T, gamma = o.linewidth;
    if (!wells && !(gamma > 0)) {
        *error = tr("%1: a wire's spectrum wants a broadening more than 0").arg(a_node->name());
        return -1;
    }
    a_spec = Spectrum();
    a_spec.size = a_activeSize;
    // Carriers of carrier c per area (wells) or length (wires) with a
    // quasi-Fermi level F (J).
    auto sheet = [&](int c, double F) {
        double n = 0;
        for (const State& s : a_states[c]) {
            const double m = s.mPar * M0, E = s.E * Q;
            const double x = c == 0 ? (F - E) / kT : (E - F) / kT;
            if (wells) n += m * kT / (M_PI * Hbar * Hbar) * softplus(x);
            else {
                // ∫ (1/πħ) √(2m/ε) f dε = (2√(2m)/πħ) ∫ f(u²) du.
                const int steps = 400;
                const double top = std::sqrt(std::max(40.0, x + 40) * kT);
                double sum = 0;
                for (int i = 0; i < steps; ++i) {
                    const double u = (i + 0.5) * top / steps;
                    sum += fermi(u * u / kT - x) * top / steps;
                }
                n += 2 * std::sqrt(2 * m) / (M_PI * Hbar) * sum;
            }
        }
        return n;
    };
    a_spec.carriers = o.density > 0;
    if (a_spec.carriers) {
        const double want = o.density * a_activeSize;
        auto level = [&](bool electrons) {
            double lo = -30 * Q, hi = 30 * Q;
            for (int it = 0; it < 200; ++it) {
                const double mid = (lo + hi) / 2;
                const double have = electrons ? sheet(0, mid) : sheet(1, mid) + sheet(2, mid);
                // (Electrons' count rises with F, holes' falls.)
                if ((have < want) == electrons) lo = mid;
                else hi = mid;
            }
            return (lo + hi) / 2;
        };
        a_spec.Fc = level(true);
        a_spec.Fv = level(false);
        if (!a_states[0].empty() && a_spec.Fc > a_states[0].back().E * Q - 3 * kT)
            a_s.log << tr("Warning: %1: the electrons' quasi-Fermi level is near the highest state found: ask for more states").arg(a_node->name());
    }
    // The transitions e_i to hh_j and lh_j: their overlaps, polarizations
    // (at the band edge: TE 3/2 and 1/2 of |Mb|², TM 0 and 2) and masses.
    struct Transition {
        double E, Ee, Eh;   // J
        double overlap;
        double te, tm;
        double me, mh, mr;   // kg
        double Ep, nr;
    };
    std::vector<Transition> transitions;
    for (std::size_t i = 0; i < a_states[0].size(); ++i) {
        const State& e = a_states[0][i];
        for (int c = 1; c < Carriers; ++c)
            for (std::size_t j = 0; j < a_states[c].size(); ++j) {
                const State& h = a_states[c][j];
                Transition t;
                t.Ee = e.E * Q;
                t.Eh = h.E * Q;
                t.E = t.Ee - t.Eh;
                const double ov = integral(e.x, h.x, {});
                t.overlap = ov * ov;
                t.te = c == 1 ? 1.5 : 0.5;
                t.tm = c == 1 ? 0.0 : 2.0;
                t.me = e.mPar * M0;
                t.mh = h.mPar * M0;
                t.mr = 1 / (1 / t.me + 1 / t.mh);
                t.Ep = e.Ep * Q;
                t.nr = o.index > 0 ? o.index : e.nr;
                if (t.overlap > 1e-10) transitions.push_back(t);
            }
    }
    a_spec.nr = o.index > 0 ? o.index : (a_states[0].empty() ? 3.5 : a_states[0].front().nr);
    // Each transition's density of states broadened (Lorentzian, half width
    // γ) and weighed by f_v - f_c along its in-plane dispersion: in cells,
    // the Lorentzian integrated exactly over each.
    const double top = std::max(60 * gamma, std::max(0.3 * Q, 30 * kT));
    const double step = std::max(1e-6 * Q, std::min(gamma > 0 ? gamma / 6 : kT / 6, kT / 6));
    const int cells = std::clamp(int(std::ceil(top / step)), 50, 20000);
    auto occupation = [&](const Transition& t, double eps) {
        if (!a_spec.carriers) return 1.0;
        const double Ec = t.Ee + t.mr / t.me * eps, Ev = t.Eh - t.mr / t.mh * eps;
        return fermi((Ev - a_spec.Fv) / kT) - fermi((Ec - a_spec.Fc) / kT);
    };
    auto lorentz = [&](double a, double b, double delta) {
        // ∫_a^b L(delta - ε) dε.
        return (std::atan((b - delta) / gamma) - std::atan((a - delta) / gamma)) / M_PI;
    };
    for (double E : o.energies) {
        double te = 0, tm = 0;
        for (const Transition& t : transitions) {
            const double delta = E - t.E;
            double dos = 0;   // 1/(J m²) wells, 1/(J m) wires
            if (wells) {
                double sum = 0;
                if (gamma > 0 && !a_spec.carriers) sum = 0.5 + std::atan(delta / gamma) / M_PI;   // (a step's, broadened: exact)
                else if (gamma > 0) {
                    for (int k = 0; k < cells; ++k) {
                        const double a = top * k / cells, b = top * (k + 1) / cells;
                        sum += occupation(t, (a + b) / 2) * lorentz(a, b, delta);
                    }
                    sum += occupation(t, top) * (0.5 - std::atan((top - delta) / gamma) / M_PI);
                } else if (delta >= 0) sum = occupation(t, delta);
                dos = t.mr / (M_PI * Hbar * Hbar) * sum;
            } else {
                // In u = √ε: each cell's mean of ε^(-1/2) is 2/(u_k + u_k+1).
                const double utop = std::sqrt(top);
                double sum = 0;
                for (int k = 0; k < cells; ++k) {
                    const double u0 = utop * k / cells, u1 = utop * (k + 1) / cells;
                    sum += occupation(t, (u0 * u0 + u1 * u1) / 2) * 2 / (u0 + u1) * lorentz(u0 * u0, u1 * u1, delta);
                }
                sum += occupation(t, top) / utop * (0.5 - std::atan((top - delta) / gamma) / M_PI);
                dos = std::sqrt(2 * t.mr) / (M_PI * Hbar) * sum;
            }
            // α = (π q² ħ / (n c ε0 m0² E)) |ê·p|² |<e|h>|² ρ / (the wells' size); |Mb|² = m0 Ep / 6.
            const double C = M_PI * Q * Q * Hbar / (t.nr * C0 * Eps0 * M0 * M0 * E) * (M0 * t.Ep / 6) * t.overlap * dos / a_activeSize;
            te += C * t.te;
            tm += C * t.tm;
        }
        a_spec.E.push_back(E);
        a_spec.te.push_back(te);
        a_spec.tm.push_back(tm);
    }
    a_s.log << tr("%1: a spectrum of %2 transitions at %3 photon energies, %4%5")
                   .arg(a_node->name())
                   .arg(transitions.size())
                   .arg(o.energies.size())
                   .arg(a_spec.carriers ? tr("the quasi-Fermi levels %1 apart").arg(formatQuantity((a_spec.Fc - a_spec.Fv) / Q, Dim::voltage()))
                                        : tr("no carriers"))
                   .arg(QString());
    selectMode(0);
    return int(a_spec.E.size());
}

void QuantumProblem::selectSpectrum(int k)
{
    a_specSelected = k;
}

void QuantumProblem::setGlobals()
{
    const QString p = field().tag + QLatin1Char('.');
    const Dim energy = Dim::of(2, 1, -2);
    const QString carrier[Carriers] = {tr("electron"), tr("heavy hole"), tr("light hole")};
    for (int c = 0; c < Carriers; ++c) {
        if (!a_on[c]) continue;
        const QString n = QLatin1String(CarrierName[c]);
        for (std::size_t k = 0; k < a_states[c].size(); ++k)
            a_s.setGlobal(p + QStringLiteral("E%1%2").arg(n).arg(k + 1), a_states[c][k].E * Q, energy,
                          tr("the %1 state %2's energy").arg(carrier[c]).arg(k + 1));
        const bool has = a_selected >= 0 && a_selected < int(a_states[c].size());
        const State* s = has ? &a_states[c][std::size_t(a_selected)] : nullptr;
        a_s.setGlobal(p + QStringLiteral("E_") + n, s ? s->E * Q : NaN, energy, tr("the %1 state's energy (the one selected)").arg(carrier[c]));
        a_s.setGlobal(p + QStringLiteral("pos_") + n, s ? s->pos : NaN, Dim::length(),
                      tr("where the %1 state is along the growth axis: its mean").arg(carrier[c]));
        a_s.setGlobal(p + QStringLiteral("bound_") + n, s ? (s->bound ? 1.0 : 0.0) : NaN, Dim(),
                      tr("1 when the %1 state is bound (inside the barriers' band edge)").arg(carrier[c]));
    }
    a_s.setGlobal(p + QStringLiteral("state"), a_selected + 1, Dim(), tr("the state's number, from the band edge"));
    // The gap the wells make, and the transitions with their overlaps.
    if (a_on[0] && !a_states[0].empty() && ((a_on[1] && !a_states[1].empty()) || (a_on[2] && !a_states[2].empty()))) {
        double top = -Inf();
        for (int c = 1; c < Carriers; ++c)
            if (a_on[c] && !a_states[c].empty()) top = std::max(top, a_states[c].front().E);
        const double gap = (a_states[0].front().E - top) * Q;
        a_s.setGlobal(p + QStringLiteral("Eg"), gap, energy, tr("the effective gap: the first electron state to the highest hole state"));
        a_s.setGlobal(p + QStringLiteral("lambda"), gap > 0 ? 2 * M_PI * Hbar * C0 / gap : NaN, Dim::length(),
                      tr("the wavelength of the effective gap: hc/Eg"));
        if (a_on[1]) {
            a_s.setGlobal(p + QStringLiteral("Edir"), a_directE * Q, energy,
                          tr("the direct transition: the lowest electron to heavy hole one of at least half the largest overlap (a well's "
                             "own e1-hh1, where a field puts the stack's lowest and highest states in different wells)"));
            a_s.setGlobal(p + QStringLiteral("Odir"), a_directO, Dim(), tr("the direct transition's overlap"));
        }
        const std::size_t most = 4;
        for (std::size_t i = 0; i < std::min(most, a_states[0].size()); ++i)
            for (int c = 1; c < Carriers; ++c) {
                if (!a_on[c]) continue;
                for (std::size_t j = 0; j < std::min(most, a_states[c].size()); ++j) {
                    const QString name = QStringLiteral("e%1%2%3").arg(i + 1).arg(QLatin1String(CarrierName[c])).arg(j + 1);
                    a_s.setGlobal(p + QStringLiteral("E") + name, (a_states[0][i].E - a_states[c][j].E) * Q, energy,
                                  tr("the transition's energy, e%1 to %2%3").arg(i + 1).arg(QLatin1String(CarrierName[c])).arg(j + 1));
                    const bool known = i < a_overlap[c].size() && j < a_overlap[c][i].size();
                    a_s.setGlobal(p + QStringLiteral("O") + name, known ? a_overlap[c][i][j] : NaN, Dim(),
                                  tr("the overlap |<e%1|%2%3>|²").arg(i + 1).arg(QLatin1String(CarrierName[c])).arg(j + 1));
                }
            }
    }
    if (a_specSelected >= 0 && a_specSelected < int(a_spec.E.size())) {
        const std::size_t k = std::size_t(a_specSelected);
        const Dim perMetre = Dim::of(-1);
        a_s.setGlobal(p + QStringLiteral("alphaTE"), a_spec.te[k], perMetre, tr("absorption coefficient of the wells, TE (in their plane)"));
        a_s.setGlobal(p + QStringLiteral("alphaTM"), a_spec.tm[k], perMetre, tr("absorption coefficient of the wells, TM (along the growth axis)"));
        a_s.setGlobal(p + QStringLiteral("gTE"), -a_spec.te[k], perMetre, tr("material gain of the wells, TE"));
        a_s.setGlobal(p + QStringLiteral("gTM"), -a_spec.tm[k], perMetre, tr("material gain of the wells, TM"));
        a_s.setGlobal(p + QStringLiteral("Ephoton"), a_spec.E[k], energy, tr("the photon's energy"));
        a_s.setGlobal(p + QStringLiteral("Lw"), a_spec.size, a_1d ? Dim::length() : Dim::of(2),
                      a_1d ? tr("the wells' thickness: the domains of the smallest gap") : tr("the wires' area: the domains of the smallest gap"));
        if (a_spec.carriers) {
            a_s.setGlobal(p + QStringLiteral("Fc"), a_spec.Fc, energy, tr("the electrons' quasi-Fermi level"));
            a_s.setGlobal(p + QStringLiteral("Fv"), a_spec.Fv, energy, tr("the holes' quasi-Fermi level"));
            a_s.setGlobal(p + QStringLiteral("dF"), a_spec.Fc - a_spec.Fv, energy,
                          tr("the quasi-Fermi levels' separation: gain below it (Bernard and Duraffourg)"));
        }
    }
}

} // namespace

std::unique_ptr<Problem> makeQuantumProblem(Solution& solution, int field)
{
    return std::make_unique<QuantumProblem>(solution, field);
}

QuantumState quantumAt(PointEvaluator& e, int fi)
{
    QuantumState s;
    const Field& f = e.solution().fields()[std::size_t(fi)];
    if (!f.fixed || f.fixed->size() < std::size_t(Header)) return s;
    const int t = e.triangle(), d = e.domain();
    if (t < 0 || d < 0 || std::size_t(d) >= f.domains.size() || !f.domains[std::size_t(d)]) return s;
    const std::vector<double>& fx = *f.fixed;
    const bool line = fx[0] == 1;
    const std::size_t n1 = std::size_t(fx[6]), nt = std::size_t(fx[8]), nd = std::size_t(fx[9]);
    const std::size_t triBase = std::size_t(Header) + n1, domBase = triBase + 9 * nt;
    if (std::size_t(t) >= nt || std::size_t(d) >= nd || fx.size() < domBase + 4 * nd) return s;
    s.defined = true;
    const double l0 = 1 - e.xi() - e.eta(), l1 = e.xi(), l2 = e.eta();
    double* edge[3] = {&s.Ec, &s.Ehh, &s.Elh};
    for (int c = 0; c < Carriers; ++c) {
        const std::size_t b = triBase + std::size_t(9 * t + 3 * c);
        *edge[c] = l0 * fx[b] + l1 * fx[b + 1] + l2 * fx[b + 2];
    }
    s.me = fx[domBase + std::size_t(4 * d)];
    s.mhh = fx[domBase + std::size_t(4 * d + 1)];
    s.mlh = fx[domBase + std::size_t(4 * d + 2)];
    s.strain = fx[domBase + std::size_t(4 * d + 3)];
    s.drawn = fx[7];
    if (f.extra.size() < std::size_t(2 * Carriers)) return s;
    for (int c = 0; c < Carriers; ++c) {
        s.E[c] = f.extra[std::size_t(c)];
        s.peak[c] = f.extra[std::size_t(Carriers + c)];
    }
    if (line) {
        // Along the line: its quadratic elements at the point's projection.
        const QPointF S(fx[2], fx[3]), E(fx[4], fx[5]);
        const double L = std::hypot(E.x() - S.x(), E.y() - S.y());
        const QPointF p = e.point() - S;
        const double z = L > 0 ? (p.x() * (E.x() - S.x()) + p.y() * (E.y() - S.y())) / L : -1;
        if (z < 0 || z > L || n1 < 3 || f.extra.size() < std::size_t(2 * Carriers) + Carriers * n1) return s;
        const double* zs = fx.data() + Header;
        const std::ptrdiff_t at = std::upper_bound(zs, zs + n1, z) - zs;
        std::size_t el = std::size_t(std::max<std::ptrdiff_t>(0, at - 1)) / 2;
        el = std::min(el, (n1 - 1) / 2 - 1);
        const double z0 = zs[2 * el], z2 = zs[2 * el + 2];
        const double xi = z2 > z0 ? std::clamp((z - z0) / (z2 - z0), 0.0, 1.0) : 0;
        const double N[3] = {(1 - xi) * (1 - 2 * xi), 4 * xi * (1 - xi), xi * (2 * xi - 1)};
        for (int c = 0; c < Carriers; ++c) {
            if (!std::isfinite(s.E[c])) continue;
            const double* v = f.extra.data() + 2 * Carriers + std::size_t(c) * n1 + 2 * el;
            s.psi[c] = N[0] * v[0] + N[1] * v[1] + N[2] * v[2];
        }
    } else {
        for (int c = 0; c < Carriers; ++c) {
            if (!std::isfinite(s.E[c])) continue;
            double u = 0, gx = 0, gy = 0;
            e.component(fi, c, u, gx, gy);
            s.psi[c] = u;
        }
    }
    return s;
}

} // namespace qucs_s::fem::detail
