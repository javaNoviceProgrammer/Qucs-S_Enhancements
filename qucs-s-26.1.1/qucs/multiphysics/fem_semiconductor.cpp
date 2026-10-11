/*
 * fem_semiconductor.cpp - semiconductors: the drift-diffusion equations
 *                         (Poisson's for the potential, continuity for
 *                         electrons and holes) on linear triangles, each
 *                         edge's carrier current Scharfetter and Gummel's
 *                         weighed as the Laplacian weighs it (edge-averaged
 *                         finite elements: an M-matrix on a Delaunay mesh);
 *                         solved together by Newton's method from
 *                         equilibrium, the contacts' voltages raised in
 *                         steps; in time (BDF), and small-signal about a
 *                         solution (the contacts' admittance matrix); a
 *                         contact's current weighed over the device where
 *                         it is resolved, not taken from its own rows
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_problem.h"
#include "fem_geometry.h"
#include "fem_mesh.h"

#include <Eigen/SparseCholesky>
#include <Eigen/SparseLU>

#include <algorithm>
#include <cmath>
#include <complex>
#include <map>
#include <optional>
#include <tuple>

namespace qucs_s::fem::detail {

namespace {

constexpr double Q = 1.602176634e-19;    // C
constexpr double KB = 1.380649e-23;      // J/K
const double Inf = std::numeric_limits<double>::infinity();

/// Bernoulli's function x / (e^x - 1) and its derivative, safe at 0 and far
/// from it.
double bern(double x)
{
    if (std::abs(x) < 1e-5) return 1 - x / 2 + x * x / 12;
    if (x > 600) return x * std::exp(-x);
    if (x < -600) return -x;
    return x / std::expm1(x);
}

double dbern(double x)
{
    if (std::abs(x) < 1e-5) return -0.5 + x / 6;
    if (x > 600) return (1 - x) * std::exp(-x);
    if (x < -600) return -1;
    const double em = std::expm1(x);
    return (em - x * (em + 1)) / (em * em);
}

/// A domain's semiconductor (or insulator) as solved.
struct Medium {
    bool semiconductor = false;
    double epsr = 1;
    double Eg = 0, chi = 0;          // J
    double Nc = 0, Nv = 0;           // 1/m³
    double mun = 0, mup = 0;         // m²/(V s): the lattice's (most)
    bool dopingMobility = false;     // Caughey and Thomas's
    double munMin = 0, nRefN = 1, alphaN = 1, mupMin = 0, nRefP = 1, alphaP = 1;
    bool saturation = false;         // high fields: Caughey and Thomas's too
    double vsatn = 1e5, vsatp = 8e4, betan = 2, betap = 1;
    bool srh = false;
    double taun = Inf, taup = Inf, Et = 0;   // Et: the trap's level above the intrinsic one (J)
    double Cn = 0, Cp = 0, B = 0;
    std::vector<Expression> generation;
    double ni = 0, dEc = 0;          // the intrinsic density; Ec - Ei (J)
};

struct Contact {
    enum Kind { Ohmic, Schottky, Gate };
    QString name;
    Kind kind = Ohmic;
    Expression V0;
    double phiB = 0;                 // a Schottky contact's barrier (J)
    double vn = 1e5, vp = 1e5;       // its recombination velocities (m/s)
    double phim = 0;                 // a gate's work function (J)
    std::vector<int> nodes;
    std::vector<double> length;      // by node: the contact's area there (m²: a length times the thickness)
    double applied = 0;              // the voltage the solution is at (V)
    double current = 0, charge = 0;  // A into the device, C on the metal
};

class SemiconductorProblem : public Problem
{
public:
    SemiconductorProblem(Solution& solution, int field) : Problem(solution, field) {}

    bool setup(QString* error) override;
    void initialize() override;
    bool solve(const SolveOptions& options, QString* error) override;
    void setGlobals() override;
    bool hasMass() const override { return true; }
    bool harmonic() const override { return true; }

private:
    struct Edge {
        int i = -1, j = -1;
        double w = 0;                // Σ cot weights (m², the thickness in)
        double wn = 0, wp = 0;       // times the elements' mobilities (the lattice's, by doping)
        double h = 0;                // its length (m)
    };
    int nodes() const { return a_n; }
    double doping(int i) const { return a_Nd[std::size_t(i)] - a_Na[std::size_t(i)]; }
    /// Equilibrium's carriers at a node of net doping \a C (no applied voltage).
    void neutral(int i, double* n, double* p) const;
    /// The contacts' voltages at the solution's time.
    bool targets(std::vector<double>& out, QString* error);
    /// The values a Dirichlet node takes (V, n, p) at the contacts' applied voltages.
    void contactValues(const std::vector<double>& applied);
    /// F(x) and (\a jac) its Jacobian - interior rows; the contacts' rows as
    /// they are (their reactions), Dirichlet rows replaced in \a system.
    void assemble(const Eigen::VectorXd& x, double mass, const std::vector<double>* history, Eigen::VectorXd& F,
                  std::vector<Eigen::Triplet<double>>* jac) const;
    /// Newton's method from \a x at the contacts' values set; how many steps
    /// in \a steps.
    bool newton(Eigen::VectorXd& x, double mass, const std::vector<double>* history, int maxSteps, int* steps, QString* why);
    /// Poisson's equation alone, the carriers at equilibrium with a Fermi
    /// level of -q \a fermi, the contacts at \a applied; from \a x's
    /// potential (\a warm) or charge neutrality's.
    bool equilibriumPoisson(Eigen::VectorXd& x, double fermi, const std::vector<double>& applied, bool warm, QString* why);
    /// Whether the contacts that hold the carriers are at one voltage (in
    /// \a fermi) and nothing makes carriers: no current flows, and the
    /// carriers are at equilibrium - Poisson's equation alone is exact.
    bool atEquilibrium(const std::vector<double>& goal, double* fermi) const;
    /// At rest: from where the solution is to the contacts' voltages, in steps.
    bool stationary(QString* why);
    bool smallSignal(double frequency, QString* why);
    /// At equilibrium: the gates' quasi-static capacitance - the carriers
    /// following, their Fermi level held at \a fermi - from Poisson's equation
    /// linearized: the low-frequency limit, well conditioned (the small-signal
    /// system is not at 1 µHz: a MOS capacitor's inversion layer is held by
    /// almost nothing).
    void quasiStatic(const Eigen::VectorXd& x, double fermi, const std::vector<double>& applied);
    /// Poisson's equation with the carriers at equilibrium (Fermi level
    /// \a fermi): its residual on the potential's unknowns (\a Fv, by \a
    /// vIndex) and Jacobian (\a J, contacts' rows as assembled when \a
    /// keepContacts, else fixed).
    void equilibriumSystem(const Eigen::VectorXd& x, double fermi, const std::vector<int>& local, const std::vector<int>& vIndex,
                           const std::vector<char>& fixed, bool keepContacts, Eigen::VectorXd& Fv, Eigen::SparseMatrix<double>& J) const;
    void store(const Eigen::VectorXd& x);
    Eigen::VectorXd stateVector() const;
    /// The contacts' currents and charges at \a x (at equilibrium, \a
    /// still: no current, exactly).
    void terminalValues(const Eigen::VectorXd& x, double mass, const std::vector<double>* history, bool still = false);
    /// By contact (empty for a gate): a weight over the nodes, 1 on its own,
    /// 0 on the other contacts' that hold carriers, between them as the
    /// carriers' conductance at \a x has it - flat where they are many. A
    /// contact's current is the edges' currents times the weight's change
    /// along them: the same as its rows' reaction where the equations hold,
    /// but not the difference of a 10^19 cm^-3 contact's two large fluxes
    /// (each edge's ~100 A per metre, its last digits pA).
    std::vector<std::vector<double>> currentWeights(const Eigen::VectorXd& x);
    /// The edge mobilities at the fields of \a x (high fields lower them).
    void updateMobilities(const Eigen::VectorXd& x);

    int a_n = 0;                       // nodes (P1 degrees of freedom)
    double a_T = 300, a_Vt = 0.025852;
    double a_scale = 1;                // a carrier scale (1/m³): the largest doping or ni
    std::vector<Medium> a_media;       // by domain
    std::vector<char> a_elemSemi;      // by triangle: of a semiconductor in the physics
    std::vector<char> a_elemOn;        // in the physics
    std::vector<int> a_nodeDomain;     // a node's semiconductor domain (-1: none)
    std::vector<double> a_box;         // by node: its share of semiconductor (m³)
    std::vector<double> a_Nd, a_Na, a_ni, a_n1, a_p1;
    std::vector<std::array<double, 9>> a_K;   // by triangle: ε0 εr ∫∇φi·∇φj (weighted)
    std::vector<Edge> a_edges;
    // By triangle of a semiconductor: its low-field mobilities, its edges
    // (k to k+1) and their cot weights, its shape functions' gradients.
    struct ElementMobility {
        double mun = 0, mup = 0;
        int edge[3] = {-1, -1, -1};
        double cot[3] = {0, 0, 0};
        double gx[3] = {0, 0, 0}, gy[3] = {0, 0, 0};
        int domain = -1;
    };
    std::vector<ElementMobility> a_elements;
    /// The edges' weights from the elements' mobilities - each saturated by
    /// its element's field at \a x when there is one (and the model asks).
    void edgeWeights(const Eigen::VectorXd* x);
    std::vector<int> a_iV, a_iN, a_iP; // by node: unknowns (-1 none)
    int a_unknowns = 0;
    std::vector<Contact> a_contacts;
    std::vector<int> a_nodeContact;    // by node (-1 none)
    std::vector<double> a_bcV, a_bcN, a_bcP;   // by node: a Dirichlet node's values
    std::vector<std::pair<int, Expression>> a_surface;   // node, charge density's expression
    std::vector<double> a_surfaceLength;                 // by entry: area (m²)
    std::vector<std::pair<int, std::array<double, 3>>> a_nodePoint;   // a node's triangle and reference point
    std::vector<double> a_generation;  // by node (1/(m³ s)), at the last solve
    bool a_saturation = false;
    double a_refDEc = 0, a_refChi = 0; // the first semiconductor's: Ec - Ei, χ (J)
    QString a_initError;
    int a_clamped = 0;
    // Small-signal: the admittance matrix by contacts (S), at a frequency.
    std::vector<std::vector<std::complex<double>>> a_Y;
    double a_frequency = 0;
    // At equilibrium: the gates' quasi-static capacitance matrix (F), by gate.
    std::vector<std::vector<double>> a_C0;
    std::vector<int> a_gates;
    // The weights' system (its pattern the mesh's: analysed once).
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> a_weightSolver;
    int a_weightSize = -1;
};

/// A band energy (eV, or V as eV per electron, or a plain number in eV)
/// in joules; NaN and why when it is not a constant.
double energyOf(const QString& text, const Scope& scope, QString* why)
{
    const Expression e = Expression::compile(text, scope);
    if (!e.isValid() || !e.isConstant()) {
        *why = e.isValid() ? tr("%1 must be a constant").arg(text) : e.error();
        return NaN;
    }
    if (e.dim() == Dim::of(2, 1, -2)) return e.constant();
    return e.constant() * Q;   // V or a plain number: eV
}

bool SemiconductorProblem::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field& f = field();
    const Model& model = a_s.model();
    const Topology& topo = a_s.topology();
    const Mesh& mesh = a_s.mesh();
    const Space& sp = *f.space;
    const Scope& constants = a_s.parameters().scope;
    QString why;
    a_n = sp.size();
    const QVector<int> domains = resolveSelection(topo, model, Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(topo.domains.size(), 0);
    for (int d : domains) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    if (sp.order() != 1) problem(tr("%1: its elements are linear (Scharfetter and Gummel's currents)").arg(a_node->name()));
    {
        const std::optional<double> t = evaluateConstant(a_node->text(QStringLiteral("T")), constants, Dim::temperature(), &why);
        if (!t || !(*t > 0)) problem(tr("%1: its temperature: %2").arg(a_node->name(), t ? tr("it must be more than 0") : why));
        else a_T = *t;
    }
    a_Vt = KB * a_T / Q;
    auto number = [&](const QString& text, const Dim& dim, const QString& what) {
        const std::optional<double> v = evaluateConstant(text, constants, dim, &why);
        if (!v) {
            problem(tr("%1: %2").arg(what, why));
            return NaN;
        }
        return *v;
    };
    const std::vector<const Node*> material = materials();
    a_media.assign(topo.domains.size(), Medium());
    std::vector<const Node*> modelOf(topo.domains.size(), nullptr);
    std::vector<const Node*> srhOf(topo.domains.size(), nullptr);
    std::vector<const Node*> augerOf(topo.domains.size(), nullptr), radiativeOf(topo.domains.size(), nullptr);
    const Dim conc = Dim::of(-3), mobility = Dim::of(0, -1, 2, 1), velocity = Dim::of(1, 0, -1);
    struct DopingDef {
        const Node* node;
        QVector<int> domains;
    };
    std::vector<DopingDef> dopings;
    std::vector<const Node*> boundaryFeatures;
    for (const Node& feature : a_node->children) {
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k) continue;
        const QVector<int> picked = resolveSelection(topo, model, k->selection, feature.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(feature.name(), why));
        const QString t = feature.type;
        if (t == QLatin1String("semiconductor/model")) for (int d : picked) modelOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("semiconductor/srh")) for (int d : picked) srhOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("semiconductor/auger")) for (int d : picked) augerOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("semiconductor/radiative")) for (int d : picked) radiativeOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("semiconductor/doping")) dopings.push_back({&feature, picked});
        else if (t == QLatin1String("semiconductor/generation")) {
            const Expression g = compile(feature.text(QStringLiteral("G")), feature, QStringLiteral("G"));
            if (g.isValid())
                for (int d : picked) a_media[std::size_t(d)].generation.push_back(g);
        } else if (k->selection == Level::Boundary) boundaryFeatures.push_back(&feature);
    }
    // Each domain's medium.
    bool referenced = false;
    for (int d : domains) {
        Medium& m = a_media[std::size_t(d)];
        const Node* mf = modelOf[std::size_t(d)];
        const Node* mat = material[std::size_t(d)];
        const QString object = topo.objectNames.value(topo.domains[std::size_t(d)].owner);
        const bool user = mf && mf->text(QStringLiteral("source")) == QLatin1String("user");
        auto text = [&](const QString& key) {
            if (user) return mf->text(key);
            return mat ? materialProperty(*mat, key, &constants) : QString();
        };
        const QString where = tr("domain %1 (%2)").arg(d + 1).arg(object);
        if (!user && !mat) {
            problem(tr("%1 has no material").arg(where));
            continue;
        }
        const QString eps = text(QStringLiteral("epsilonr"));
        if (eps.trimmed().isEmpty()) problem(tr("%1: its material has no relative permittivity").arg(where));
        else m.epsr = number(eps, Dim(), where);
        m.semiconductor = !text(QStringLiteral("Eg")).trimmed().isEmpty();
        if (!m.semiconductor) continue;
        m.Eg = energyOf(text(QStringLiteral("Eg")), constants, &why);
        if (!std::isfinite(m.Eg)) problem(tr("%1: its band gap: %2").arg(where, why));
        const QString chi = text(QStringLiteral("chi"));
        m.chi = chi.trimmed().isEmpty() ? 4.05 * Q : energyOf(chi, constants, &why);
        for (const auto& [key, member, dim] : {std::tuple{QStringLiteral("Nc"), &Medium::Nc, conc}, std::tuple{QStringLiteral("Nv"), &Medium::Nv, conc},
                                               std::tuple{QStringLiteral("mun"), &Medium::mun, mobility},
                                               std::tuple{QStringLiteral("mup"), &Medium::mup, mobility}}) {
            const QString v = text(key);
            if (v.trimmed().isEmpty()) problem(tr("%1: its material has no %2").arg(where, key));
            else m.*member = number(v, dim, where);
        }
        if (!(m.Nc > 0) || !(m.Nv > 0) || !(m.Eg > 0)) {
            problem(tr("%1: its band gap and effective densities of states must be more than 0").arg(where));
            continue;
        }
        m.ni = std::sqrt(m.Nc * m.Nv) * std::exp(-m.Eg / (2 * KB * a_T));
        m.dEc = m.Eg / 2 + KB * a_T / 2 * std::log(m.Nc / m.Nv);
        if (!referenced) {
            a_refDEc = m.dEc;
            a_refChi = m.chi;
            referenced = true;
        }
        if (mf && mf->text(QStringLiteral("mobility")) == QLatin1String("doping")) {
            m.dopingMobility = true;
            m.munMin = number(mf->text(QStringLiteral("munmin")), mobility, where);
            m.nRefN = number(mf->text(QStringLiteral("nrefn")), conc, where);
            m.alphaN = number(mf->text(QStringLiteral("alphan")), Dim(), where);
            m.mupMin = number(mf->text(QStringLiteral("mupmin")), mobility, where);
            m.nRefP = number(mf->text(QStringLiteral("nrefp")), conc, where);
            m.alphaP = number(mf->text(QStringLiteral("alphap")), Dim(), where);
        }
        if (mf && mf->flag(QStringLiteral("saturation"))) {
            m.saturation = a_saturation = true;
            m.vsatn = number(mf->text(QStringLiteral("vsatn")), velocity, where);
            m.vsatp = number(mf->text(QStringLiteral("vsatp")), velocity, where);
        }
        if (const Node* r = srhOf[std::size_t(d)]) {
            m.srh = true;
            const bool own = r->text(QStringLiteral("source")) == QLatin1String("user");
            auto rt = [&](const QString& key) { return own ? r->text(key) : mat ? materialProperty(*mat, key, &constants) : QString(); };
            const QString tn = rt(QStringLiteral("taun")), tp = rt(QStringLiteral("taup"));
            if (tn.trimmed().isEmpty() || tp.trimmed().isEmpty()) problem(tr("%1: its material has no carrier lifetimes (taun, taup)").arg(where));
            else {
                m.taun = number(tn, Dim::of(0, 0, 1), where);
                m.taup = number(tp, Dim::of(0, 0, 1), where);
            }
            m.Et = energyOf(r->text(QStringLiteral("Et")), constants, &why);
            if (!std::isfinite(m.Et)) m.Et = 0;
        }
        if (const Node* a = augerOf[std::size_t(d)]) {
            m.Cn = number(a->text(QStringLiteral("Cn")), Dim::of(6, 0, -1), where);
            m.Cp = number(a->text(QStringLiteral("Cp")), Dim::of(6, 0, -1), where);
        }
        if (const Node* r = radiativeOf[std::size_t(d)]) m.B = number(r->text(QStringLiteral("B")), Dim::of(3, 0, -1), where);
    }
    if (!referenced && !domains.isEmpty()) problem(tr("%1: none of its domains is a semiconductor (a material with a band gap Eg)").arg(a_node->name()));
    // Elements and nodes.
    const int nt = sp.elements();
    a_elemSemi.assign(std::size_t(nt), 0);
    a_elemOn.assign(std::size_t(nt), 0);
    a_nodeDomain.assign(std::size_t(a_n), -1);
    std::vector<char> nodeOn(std::size_t(a_n), 0);
    a_box.assign(std::size_t(a_n), 0);
    a_K.assign(std::size_t(nt), {});
    std::map<std::pair<int, int>, int> edgeIndex;
    a_edges.clear();
    const double scale = mesh.unitScale;
    for (int t = 0; t < nt; ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        a_elemOn[std::size_t(t)] = 1;
        const Medium& m = a_media[std::size_t(d)];
        const int* dof = sp.dofs(t);
        QPointF x[3];
        for (int k = 0; k < 3; ++k) {
            x[k] = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * scale;
            nodeOn[std::size_t(dof[k])] = 1;
        }
        const double det = (x[1].x() - x[0].x()) * (x[2].y() - x[0].y()) - (x[2].x() - x[0].x()) * (x[1].y() - x[0].y());
        const double area = std::abs(det) / 2;
        const double weight = a_s.weight((x[0] + x[1] + x[2]) / 3.0);
        // ∇φk = perpendicular of the opposite edge over 2 A (signed).
        double gx[3], gy[3];
        for (int k = 0; k < 3; ++k) {
            const QPointF& a = x[(k + 1) % 3];
            const QPointF& b = x[(k + 2) % 3];
            gx[k] = (a.y() - b.y()) / det;
            gy[k] = (b.x() - a.x()) / det;
        }
        std::array<double, 9>& K = a_K[std::size_t(t)];
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) K[std::size_t(3 * a + b)] = Eps0 * m.epsr * area * weight * (gx[a] * gx[b] + gy[a] * gy[b]);
        if (!m.semiconductor) continue;
        a_elemSemi[std::size_t(t)] = 1;
        for (int k = 0; k < 3; ++k) {
            a_box[std::size_t(dof[k])] += area * weight / 3;
            if (a_nodeDomain[std::size_t(dof[k])] < 0) a_nodeDomain[std::size_t(dof[k])] = d;
        }
        for (int k = 0; k < 3; ++k) {
            const int a = dof[k], b = dof[(k + 1) % 3];
            const auto key = std::minmax(a, b);
            auto it = edgeIndex.find({key.first, key.second});
            if (it == edgeIndex.end()) {
                it = edgeIndex.emplace(std::pair{key.first, key.second}, int(a_edges.size())).first;
                Edge e;
                e.i = key.first;
                e.j = key.second;
                e.h = std::hypot(x[(k + 1) % 3].x() - x[k].x(), x[(k + 1) % 3].y() - x[k].y());
                a_edges.push_back(e);
            }
            // (Its weights: edgeWeights(), once the mobilities are known.)
        }
    }
    // Doping, by node: each feature's in its share of the node's box (a node
    // on a junction between two domains doped differently takes their mean).
    a_Nd.assign(std::size_t(a_n), 0);
    a_Na.assign(std::size_t(a_n), 0);
    a_nodePoint.assign(std::size_t(a_n), {-1, {0, 0, 0}});
    struct Share {
        int domain, triangle, corner;
        double area;
    };
    std::vector<std::vector<Share>> shares(static_cast<std::size_t>(a_n));
    for (int t = 0; t < nt; ++t) {
        if (!a_elemOn[std::size_t(t)]) continue;
        const int* dof = sp.dofs(t);
        static const double ref[3][2] = {{0, 0}, {1, 0}, {0, 1}};
        for (int k = 0; k < 3; ++k) {
            const int i = dof[k];
            if (a_nodePoint[std::size_t(i)].first < 0 || (a_elemSemi[std::size_t(t)] && !a_elemSemi[std::size_t(a_nodePoint[std::size_t(i)].first)]))
                a_nodePoint[std::size_t(i)] = {t, {ref[k][0], ref[k][1], 0}};
            if (!a_elemSemi[std::size_t(t)]) continue;
            const QPointF a = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][0])], b = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][1])],
                          c = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][2])];
            const double area = std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y()));
            shares[std::size_t(i)].push_back({mesh.domain[std::size_t(t)], t, k, area});
        }
    }
    PointEvaluator ev(a_s);
    Expression::Options inUnit;
    inUnit.lengthUnit = topo.unitScale;
    inUnit.numbersAreLengths = true;
    for (const DopingDef& dd : dopings) {
        const Node& df = *dd.node;
        const bool donor = df.text(QStringLiteral("dopant")) == QLatin1String("donor");
        const QString profile = df.text(QStringLiteral("profile"));
        std::vector<char> on(topo.domains.size(), 0);
        for (int d : dd.domains) on[std::size_t(d)] = 1;
        Expression N;
        double N0 = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, sigma = 0;
        if (profile == QLatin1String("expression")) N = compile(df.text(QStringLiteral("N")), df, QStringLiteral("N"));
        else {
            N0 = number(df.text(QStringLiteral("N0")), conc, df.name());
            if (profile == QLatin1String("box")) {
                auto length = [&](const QJsonValue& v) {
                    const Expression e = Expression::compile(v.toVariant().toString(), constants, inUnit);
                    if (!e.isValid() || !e.isConstant()) {
                        problem(tr("%1: %2").arg(df.name(), e.isValid() ? tr("its box must be constants") : e.error()));
                        return 0.0;
                    }
                    return e.constant() * topo.unitScale;
                };
                const QJsonArray base = df.value(QStringLiteral("position")).toArray(), size = df.value(QStringLiteral("size")).toArray();
                x0 = length(base.at(0));
                y0 = length(base.at(1));
                x1 = x0 + length(size.at(0));
                y1 = y0 + length(size.at(1));
                if (x1 < x0) std::swap(x0, x1);
                if (y1 < y0) std::swap(y0, y1);
                const double dj = number(df.text(QStringLiteral("dj")), Dim::length(), df.name());
                const double Nb = number(df.text(QStringLiteral("Nb")), conc, df.name());
                if (!(dj > 0) || !(Nb > 0) || !(N0 > Nb)) problem(tr("%1: its junction depth, background and peak must be more than 0, the peak more than the background").arg(df.name()));
                else sigma = dj / std::sqrt(std::log(N0 / Nb));
            }
        }
        static const double ref[3][2] = {{0, 0}, {1, 0}, {0, 1}};
        for (int i = 0; i < a_n; ++i) {
            double inside = 0, all = 0;
            const Share* where = nullptr;
            for (const Share& sh : shares[std::size_t(i)]) {
                all += sh.area;
                if (on[std::size_t(sh.domain)]) {
                    inside += sh.area;
                    where = &sh;
                }
            }
            if (!where || !(all > 0)) continue;
            const double frac = inside / all;
            double v = 0;
            if (profile == QLatin1String("expression")) {
                if (!N.isValid()) continue;
                ev.moveTo(where->triangle, ref[where->corner][0], ref[where->corner][1]);
                v = ev.eval(N);
            } else if (profile == QLatin1String("box")) {
                const QPointF p = sp.point(i);
                const double dx = std::max({x0 - p.x(), 0.0, p.x() - x1}), dy = std::max({y0 - p.y(), 0.0, p.y() - y1});
                const double dist = std::hypot(dx, dy);
                v = sigma > 0 ? N0 * std::exp(-(dist / sigma) * (dist / sigma)) : (dist == 0 ? N0 : 0);
            } else {
                v = N0;
            }
            if (!std::isfinite(v) || v < 0) continue;
            (donor ? a_Nd : a_Na)[std::size_t(i)] += frac * v;
        }
    }
    a_ni.assign(std::size_t(a_n), 0);
    a_n1.assign(std::size_t(a_n), 0);
    a_p1.assign(std::size_t(a_n), 0);
    a_scale = 0;
    for (int i = 0; i < a_n; ++i) {
        const int d = a_nodeDomain[std::size_t(i)];
        if (d < 0) continue;
        const Medium& m = a_media[std::size_t(d)];
        a_ni[std::size_t(i)] = m.ni;
        a_n1[std::size_t(i)] = m.ni * std::exp(m.Et / (KB * a_T));
        a_p1[std::size_t(i)] = m.ni * std::exp(-m.Et / (KB * a_T));
        a_scale = std::max({a_scale, std::abs(doping(i)), m.ni});
    }
    if (!(a_scale > 0)) a_scale = 1e16;
    // Mobilities by element (doping: the element's nodes' mean), then by edge.
    std::vector<double> mun(std::size_t(nt), 0), mup(std::size_t(nt), 0);
    for (int t = 0; t < nt; ++t) {
        if (!a_elemSemi[std::size_t(t)]) continue;
        const Medium& m = a_media[std::size_t(mesh.domain[std::size_t(t)])];
        const int* dof = sp.dofs(t);
        double N = 0;
        for (int k = 0; k < 3; ++k) N += (a_Nd[std::size_t(dof[k])] + a_Na[std::size_t(dof[k])]) / 3;
        mun[std::size_t(t)] = m.mun;
        mup[std::size_t(t)] = m.mup;
        if (m.dopingMobility) {
            mun[std::size_t(t)] = m.munMin + (m.mun - m.munMin) / (1 + std::pow(N / m.nRefN, m.alphaN));
            mup[std::size_t(t)] = m.mupMin + (m.mup - m.mupMin) / (1 + std::pow(N / m.nRefP, m.alphaP));
        }
    }
    a_elements.assign(std::size_t(nt), ElementMobility());
    for (int t = 0; t < nt; ++t) {
        if (!a_elemSemi[std::size_t(t)]) continue;
        ElementMobility& em = a_elements[std::size_t(t)];
        em.mun = mun[std::size_t(t)];
        em.mup = mup[std::size_t(t)];
        em.domain = mesh.domain[std::size_t(t)];
        const int* dof = sp.dofs(t);
        QPointF x[3];
        for (int k = 0; k < 3; ++k) x[k] = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * scale;
        const double weight = a_s.weight((x[0] + x[1] + x[2]) / 3.0);
        const double det = (x[1].x() - x[0].x()) * (x[2].y() - x[0].y()) - (x[2].x() - x[0].x()) * (x[1].y() - x[0].y());
        for (int k = 0; k < 3; ++k) {
            const auto key = std::minmax(dof[k], dof[(k + 1) % 3]);
            em.edge[k] = edgeIndex.at({key.first, key.second});
            // The edge's cot weight from this triangle: half the cotangent of
            // the angle opposite it.
            const QPointF u = x[k] - x[(k + 2) % 3], v = x[(k + 1) % 3] - x[(k + 2) % 3];
            const double cot = (u.x() * v.x() + u.y() * v.y()) / std::abs(u.x() * v.y() - u.y() * v.x());
            em.cot[k] = 0.5 * cot * weight;
            const QPointF& a = x[(k + 1) % 3];
            const QPointF& b = x[(k + 2) % 3];
            em.gx[k] = (a.y() - b.y()) / det;
            em.gy[k] = (b.x() - a.x()) / det;
        }
    }
    edgeWeights(nullptr);
    a_clamped = 0;
    for (const Edge& e : a_edges)
        if (e.w <= 0) ++a_clamped;
    // Contacts and surface charges.
    a_contacts.clear();
    a_nodeContact.assign(std::size_t(a_n), -1);
    a_surface.clear();
    a_surfaceLength.clear();
    for (const Node* bf : boundaryFeatures) {
        const QString t = bf->type;
        const QVector<int> picked = resolveSelection(topo, model, Level::Boundary, bf->selection(), &why);
        std::vector<char> on(topo.boundaries.size(), 0);
        for (int b : picked) on[std::size_t(b)] = 1;
        // Its nodes, and each one's share of its length (m², weighted).
        std::map<int, double> share;
        for (const Mesh::Edge& me : mesh.edges) {
            if (me.boundary < 0 || !on[std::size_t(me.boundary)]) continue;
            const QPointF a = mesh.nodes[std::size_t(me.a)] * scale, b = mesh.nodes[std::size_t(me.b)] * scale;
            const double len = std::hypot(b.x() - a.x(), b.y() - a.y()) * a_s.weight((a + b) / 2.0);
            if (!nodeOn[std::size_t(me.a)] && !nodeOn[std::size_t(me.b)]) continue;
            share[me.a] += len / 2;
            share[me.b] += len / 2;
        }
        if (t == QLatin1String("semiconductor/charge")) {
            const Expression e = compile(bf->text(QStringLiteral("rhos")), *bf, QStringLiteral("rhos"));
            if (!e.isValid()) continue;
            for (const auto& [node, len] : share) {
                a_surface.emplace_back(node, e);
                a_surfaceLength.push_back(len);
            }
            continue;
        }
        if (t != QLatin1String("semiconductor/ohmic") && t != QLatin1String("semiconductor/schottky") && t != QLatin1String("semiconductor/gate"))
            continue;
        Contact c;
        c.name = bf->text(QStringLiteral("name"));
        c.kind = t == QLatin1String("semiconductor/ohmic") ? Contact::Ohmic : t == QLatin1String("semiconductor/gate") ? Contact::Gate : Contact::Schottky;
        c.V0 = compile(bf->text(QStringLiteral("V0")), *bf, QStringLiteral("V0"), false);
        if (c.kind == Contact::Schottky) {
            c.phiB = energyOf(bf->text(QStringLiteral("phiB")), constants, &why);
            if (!std::isfinite(c.phiB)) problem(tr("%1: its barrier: %2").arg(bf->name(), why));
            c.vn = number(bf->text(QStringLiteral("vn")), velocity, bf->name());
            c.vp = number(bf->text(QStringLiteral("vp")), velocity, bf->name());
        }
        if (c.kind == Contact::Gate) {
            c.phim = energyOf(bf->text(QStringLiteral("phim")), constants, &why);
            if (!std::isfinite(c.phim)) problem(tr("%1: its work function: %2").arg(bf->name(), why));
        }
        for (const Contact& other : a_contacts)
            if (other.name == c.name) problem(tr("two contacts are named %1").arg(c.name));
        const int index = int(a_contacts.size());
        for (const auto& [node, len] : share) {
            if (a_nodeContact[std::size_t(node)] >= 0) continue;   // (a corner: the first contact's)
            if (c.kind == Contact::Gate && a_nodeDomain[std::size_t(node)] >= 0 && c.nodes.empty())
                a_s.log << tr("Warning: %1 touches a semiconductor: a gate is on an insulator (a metal on a semiconductor is a Schottky "
                              "or ohmic contact); its potential is fixed there")
                               .arg(bf->name());
            if ((c.kind == Contact::Ohmic || c.kind == Contact::Schottky) && a_nodeDomain[std::size_t(node)] < 0) continue;
            a_nodeContact[std::size_t(node)] = index;
            c.nodes.push_back(node);
            c.length.push_back(len);
        }
        if (c.nodes.empty()) problem(tr("%1 is on no boundary of %2's %3").arg(bf->name(), a_node->name(),
                                                                                c.kind == Contact::Gate ? tr("domains") : tr("semiconductors")));
        a_contacts.push_back(c);
    }
    bool carriersHeld = false;
    for (const Contact& c : a_contacts) carriersHeld = carriersHeld || c.kind != Contact::Gate;
    if (!carriersHeld && referenced) problem(tr("%1 has no metal contact on a semiconductor (ohmic or Schottky): its carriers' level is not set").arg(a_node->name()));
    // The unknowns: V where the physics is, n and p on its semiconductors.
    a_iV.assign(std::size_t(a_n), -1);
    a_iN.assign(std::size_t(a_n), -1);
    a_iP.assign(std::size_t(a_n), -1);
    a_unknowns = 0;
    for (int i = 0; i < a_n; ++i)
        if (nodeOn[std::size_t(i)]) a_iV[std::size_t(i)] = a_unknowns++;
    for (int i = 0; i < a_n; ++i)
        if (a_nodeDomain[std::size_t(i)] >= 0) {
            a_iN[std::size_t(i)] = a_unknowns++;
            a_iP[std::size_t(i)] = a_unknowns++;
        }
    a_bcV.assign(std::size_t(a_n), 0);
    a_bcN.assign(std::size_t(a_n), 0);
    a_bcP.assign(std::size_t(a_n), 0);
    // The field's constant parts: the doping; what the variables read.
    f.values.resize(std::size_t(5) * std::size_t(a_n), 0.0);
    for (int i = 0; i < a_n; ++i) {
        f.values[std::size_t(3 * a_n + i)] = a_Nd[std::size_t(i)];
        f.values[std::size_t(4 * a_n + i)] = a_Na[std::size_t(i)];
    }
    // extra: Vt, T, then each triangle's µn, µp, Ec - Ei, Eg, ni, τn, τp, Et, Cn, Cp, B
    // (NaN where it is no semiconductor).
    f.extra.assign(std::size_t(2 + 11 * nt), NaN);
    f.extra[0] = a_Vt;
    f.extra[1] = a_T;
    for (int t = 0; t < nt; ++t) {
        if (!a_elemSemi[std::size_t(t)]) continue;
        const Medium& m = a_media[std::size_t(mesh.domain[std::size_t(t)])];
        double* e = &f.extra[std::size_t(2 + 11 * t)];
        e[0] = mun[std::size_t(t)];
        e[1] = mup[std::size_t(t)];
        e[2] = m.dEc;
        e[3] = m.Eg;
        e[4] = m.ni;
        e[5] = m.srh ? m.taun : Inf;
        e[6] = m.srh ? m.taup : Inf;
        e[7] = m.Et;
        e[8] = m.Cn;
        e[9] = m.Cp;
        e[10] = m.B;
    }
    Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    coef.c.assign(topo.domains.size(), Expression());
    for (int d : domains) coef.c[std::size_t(d)] = Expression::compile(QString::number(a_media[std::size_t(d)].epsr, 'g', 17), a_s.pointScope());
    // Contacts as the field's terminals (their applied voltages kept with it).
    if (f.terminals.size() != a_contacts.size()) {
        f.terminals.clear();
        for (const Contact& c : a_contacts) {
            Field::Terminal term;
            term.name = c.name;
            f.terminals.push_back(term);
        }
    }
    // (Kept with them: the voltage applied, the current, the metal's charge.)
    for (std::size_t k = 0; k < a_contacts.size(); ++k)
        if (f.terminals[k].name == a_contacts[k].name) {
            a_contacts[k].applied = f.terminals[k].potential;
            a_contacts[k].current = f.terminals[k].charge;
            a_contacts[k].charge = f.terminals[k].chargeIm;
        }
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    if (a_clamped > 0) {
        const QString line = tr("%1: %2 of %3 edges not Delaunay (their currents left out)").arg(a_node->name()).arg(a_clamped).arg(a_edges.size());
        if (!a_s.log.contains(line)) a_s.log << line;
    }
    return true;
}

void SemiconductorProblem::neutral(int i, double* n, double* p) const
{
    const double C = doping(i), ni = a_ni[std::size_t(i)];
    const double half = std::abs(C) / 2;
    const double major = half + std::sqrt(half * half + ni * ni);
    const double minor = ni * ni / major;
    *n = C >= 0 ? major : minor;
    *p = C >= 0 ? minor : major;
}

bool SemiconductorProblem::targets(std::vector<double>& out, QString* error)
{
    out.assign(a_contacts.size(), 0);
    PointEvaluator ev(a_s);
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        const Contact& c = a_contacts[k];
        if (!c.V0.isValid()) {
            *error = tr("%1: the voltage of %2 is not valid").arg(a_node->name(), c.name);
            return false;
        }
        double v = c.V0.isConstant() ? c.V0.constant() : NaN;
        if (!c.V0.isConstant()) {
            const auto& [t, r] = a_nodePoint[std::size_t(c.nodes.front())];
            ev.moveTo(t, r[0], r[1]);
            v = ev.eval(c.V0);
        }
        if (!std::isfinite(v)) {
            *error = tr("%1: the voltage of %2 is not a number").arg(a_node->name(), c.name);
            return false;
        }
        out[k] = v;
    }
    return true;
}

void SemiconductorProblem::contactValues(const std::vector<double>& applied)
{
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        const Contact& c = a_contacts[k];
        for (int i : c.nodes) {
            const double va = applied[k];
            if (c.kind == Contact::Gate) {
                // The metal's Fermi level at -q Va, its vacuum level Φm above.
                a_bcV[std::size_t(i)] = va - (c.phim - a_refChi - a_refDEc) / Q;
                continue;
            }
            const Medium& m = a_media[std::size_t(a_nodeDomain[std::size_t(i)])];
            if (c.kind == Contact::Schottky) {
                a_bcV[std::size_t(i)] = va + (m.dEc - c.phiB) / Q;
                continue;
            }
            double n = 0, p = 0;
            neutral(i, &n, &p);
            a_bcN[std::size_t(i)] = n;
            a_bcP[std::size_t(i)] = p;
            a_bcV[std::size_t(i)] = va + a_Vt * std::log(n / a_ni[std::size_t(i)]);
        }
    }
}

void SemiconductorProblem::edgeWeights(const Eigen::VectorXd* x)
{
    for (Edge& e : a_edges) e.wn = e.wp = 0;
    std::vector<double> geometric(a_edges.size(), 0.0);
    const Space& sp = *a_s.fields()[std::size_t(a_fi)].space;
    for (int t = 0; t < int(a_elements.size()); ++t) {
        const ElementMobility& em = a_elements[std::size_t(t)];
        if (em.domain < 0) continue;
        double mun = em.mun, mup = em.mup;
        const Medium& m = a_media[std::size_t(em.domain)];
        if (x && m.saturation) {
            // µ(E) = µ0 / (1 + (µ0 E / vsat)^β)^(1/β), E the element's field
            // (lagged a Newton step).
            const int* dof = sp.dofs(t);
            double ex = 0, ey = 0;
            for (int k = 0; k < 3; ++k) {
                const double v = (*x)(a_iV[std::size_t(dof[k])]);
                ex += em.gx[k] * v;
                ey += em.gy[k] * v;
            }
            const double E = std::hypot(ex, ey);
            auto reduce = [&](double mu, double vsat, double beta) { return mu / std::pow(1 + std::pow(mu * E / vsat, beta), 1 / beta); };
            mun = reduce(mun, m.vsatn, m.betan);
            mup = reduce(mup, m.vsatp, m.betap);
        }
        for (int k = 0; k < 3; ++k) {
            Edge& e = a_edges[std::size_t(em.edge[k])];
            e.wn += em.cot[k] * mun;
            e.wp += em.cot[k] * mup;
            geometric[std::size_t(em.edge[k])] += em.cot[k];
        }
    }
    for (std::size_t k = 0; k < a_edges.size(); ++k) {
        Edge& e = a_edges[k];
        // (Not Delaunay there: a negative weight would make the carriers
        // oscillate; it is left out.)
        e.w = std::max(geometric[k], 0.0);
        e.wn = e.w > 0 ? std::max(e.wn, 0.0) * Q * a_Vt : 0.0;
        e.wp = e.w > 0 ? std::max(e.wp, 0.0) * Q * a_Vt : 0.0;
    }
}

void SemiconductorProblem::updateMobilities(const Eigen::VectorXd& x)
{
    if (a_saturation) edgeWeights(&x);
}

void SemiconductorProblem::assemble(const Eigen::VectorXd& x, double mass, const std::vector<double>* history, Eigen::VectorXd& F,
                                    std::vector<Eigen::Triplet<double>>* jac) const
{
    const Space& sp = *a_s.fields()[std::size_t(a_fi)].space;
    F = Eigen::VectorXd::Zero(a_unknowns);
    if (jac) jac->clear();
    auto add = [&](int r, int c, double v) {
        if (jac && r >= 0 && c >= 0) jac->emplace_back(r, c, v);
    };
    auto V = [&](int i) { return x(a_iV[std::size_t(i)]); };
    auto nn = [&](int i) { return a_iN[std::size_t(i)] >= 0 ? x(a_iN[std::size_t(i)]) : 0.0; };
    auto pp = [&](int i) { return a_iP[std::size_t(i)] >= 0 ? x(a_iP[std::size_t(i)]) : 0.0; };
    // Poisson: -Σ K V.
    for (int t = 0; t < sp.elements(); ++t) {
        if (!a_elemOn[std::size_t(t)]) continue;
        const int* dof = sp.dofs(t);
        const std::array<double, 9>& K = a_K[std::size_t(t)];
        for (int a = 0; a < 3; ++a) {
            const int r = a_iV[std::size_t(dof[a])];
            for (int b = 0; b < 3; ++b) {
                const double k = K[std::size_t(3 * a + b)];
                F(r) -= k * V(dof[b]);
                add(r, a_iV[std::size_t(dof[b])], -k);
            }
        }
    }
    // The carriers' currents along the edges.
    for (const Edge& e : a_edges) {
        if (e.w <= 0) continue;
        const int i = e.i, j = e.j;
        const double d = (V(j) - V(i)) / a_Vt;
        const double bp = bern(d), bm = bern(-d), dbp = dbern(d), dbm = dbern(-d);
        const int ni = a_iN[std::size_t(i)], nj = a_iN[std::size_t(j)], pi = a_iP[std::size_t(i)], pj = a_iP[std::size_t(j)];
        const int vi = a_iV[std::size_t(i)], vj = a_iV[std::size_t(j)];
        {
            const double fn = e.wn * (nn(j) * bp - nn(i) * bm);
            F(ni) += fn;
            F(nj) -= fn;
            const double dnj = e.wn * bp, dni = -e.wn * bm;
            const double dvj = e.wn * (nn(j) * dbp + nn(i) * dbm) / a_Vt;
            for (const auto& [row, sign] : {std::pair{ni, 1.0}, std::pair{nj, -1.0}}) {
                add(row, nj, sign * dnj);
                add(row, ni, sign * dni);
                add(row, vj, sign * dvj);
                add(row, vi, -sign * dvj);
            }
        }
        {
            const double fp = e.wp * (pp(i) * bp - pp(j) * bm);
            F(pi) += fp;
            F(pj) -= fp;
            const double dpi = e.wp * bp, dpj = -e.wp * bm;
            const double dvj = e.wp * (pp(i) * dbp + pp(j) * dbm) / a_Vt;
            for (const auto& [row, sign] : {std::pair{pi, 1.0}, std::pair{pj, -1.0}}) {
                add(row, pi, sign * dpi);
                add(row, pj, sign * dpj);
                add(row, vj, sign * dvj);
                add(row, vi, -sign * dvj);
            }
        }
    }
    // At each semiconductor node: the space charge, recombination, generation,
    // the carriers' change in time.
    const std::size_t N = std::size_t(a_n);
    for (int i = 0; i < a_n; ++i) {
        const int d = a_nodeDomain[std::size_t(i)];
        if (d < 0) continue;
        const Medium& m = a_media[std::size_t(d)];
        const double A = a_box[std::size_t(i)];
        const int rv = a_iV[std::size_t(i)], rn = a_iN[std::size_t(i)], rp = a_iP[std::size_t(i)];
        const double n = nn(i), p = pp(i), ni = a_ni[std::size_t(i)];
        F(rv) += Q * A * (p - n + doping(i));
        add(rv, rn, -Q * A);
        add(rv, rp, Q * A);
        double R = 0, dRn = 0, dRp = 0;
        const double excess = n * p - ni * ni;
        if (m.srh) {
            const double D = m.taup * (n + a_n1[std::size_t(i)]) + m.taun * (p + a_p1[std::size_t(i)]);
            R += excess / D;
            dRn += (p * D - excess * m.taup) / (D * D);
            dRp += (n * D - excess * m.taun) / (D * D);
        }
        if (m.Cn != 0 || m.Cp != 0) {
            const double c = m.Cn * n + m.Cp * p;
            R += c * excess;
            dRn += m.Cn * excess + c * p;
            dRp += m.Cp * excess + c * n;
        }
        if (m.B != 0) {
            R += m.B * excess;
            dRn += m.B * p;
            dRp += m.B * n;
        }
        const double G = a_generation.empty() ? 0.0 : a_generation[std::size_t(i)];
        F(rn) -= Q * A * (R - G);
        F(rp) += Q * A * (R - G);
        add(rn, rn, -Q * A * dRn);
        add(rn, rp, -Q * A * dRp);
        add(rp, rn, Q * A * dRn);
        add(rp, rp, Q * A * dRp);
        if (mass > 0 && history) {
            F(rn) -= Q * A * (mass * n - (*history)[N + std::size_t(i)]);
            F(rp) += Q * A * (mass * p - (*history)[2 * N + std::size_t(i)]);
            add(rn, rn, -Q * A * mass);
            add(rp, rp, Q * A * mass);
        }
    }
    // Surface charges.
    for (std::size_t k = 0; k < a_surface.size(); ++k) {
        const int i = a_surface[k].first;
        const Expression& e = a_surface[k].second;
        double rho = 0;
        if (e.isConstant()) rho = e.constant();
        else {
            PointEvaluator ev(a_s);
            const auto& [t, r] = a_nodePoint[std::size_t(i)];
            ev.moveTo(t, r[0], r[1]);
            rho = ev.eval(e);
        }
        if (std::isfinite(rho)) F(a_iV[std::size_t(i)]) += rho * a_surfaceLength[k];
    }
    // Schottky contacts: thermionic emission over the barrier.
    for (const Contact& c : a_contacts) {
        if (c.kind != Contact::Schottky) continue;
        for (std::size_t k = 0; k < c.nodes.size(); ++k) {
            const int i = c.nodes[k];
            const Medium& m = a_media[std::size_t(a_nodeDomain[std::size_t(i)])];
            const double neq = m.Nc * std::exp(-c.phiB / (KB * a_T)), peq = m.ni * m.ni / neq;
            const double L = c.length[k];
            const int rn = a_iN[std::size_t(i)], rp = a_iP[std::size_t(i)];
            F(rn) -= Q * c.vn * (nn(i) - neq) * L;
            add(rn, rn, -Q * c.vn * L);
            F(rp) += Q * c.vp * (pp(i) - peq) * L;
            add(rp, rp, Q * c.vp * L);
        }
    }
}

bool SemiconductorProblem::newton(Eigen::VectorXd& x, double mass, const std::vector<double>* history, int maxSteps, int* steps, QString* why)
{
    // Unknowns in units of Vt and of each node's own carriers (a relative
    // change: minority carriers 10^14 below the majority keep their digits);
    // each row divided by its largest entry.
    Eigen::VectorXd colScale(a_unknowns);
    auto carrierScales = [&]() {
        for (int i = 0; i < a_n; ++i) {
            if (a_iV[std::size_t(i)] >= 0) colScale(a_iV[std::size_t(i)]) = a_Vt;
            for (int k : {a_iN[std::size_t(i)], a_iP[std::size_t(i)]})
                if (k >= 0) colScale(k) = std::max(std::abs(x(k)), 1e-20 * a_scale);
        }
    };
    std::vector<char> fixed(std::size_t(a_unknowns), 0);
    Eigen::VectorXd target = Eigen::VectorXd::Zero(a_unknowns);
    for (const Contact& c : a_contacts)
        for (int i : c.nodes) {
            fixed[std::size_t(a_iV[std::size_t(i)])] = 1;
            target(a_iV[std::size_t(i)]) = a_bcV[std::size_t(i)];
            if (c.kind == Contact::Ohmic) {
                fixed[std::size_t(a_iN[std::size_t(i)])] = fixed[std::size_t(a_iP[std::size_t(i)])] = 1;
                target(a_iN[std::size_t(i)]) = a_bcN[std::size_t(i)];
                target(a_iP[std::size_t(i)]) = a_bcP[std::size_t(i)];
            }
        }
    for (int k = 0; k < a_unknowns; ++k)
        if (fixed[std::size_t(k)]) x(k) = target(k);
    Eigen::VectorXd F;
    std::vector<Eigen::Triplet<double>> trip;
    Eigen::SparseLU<Eigen::SparseMatrix<double>> lu;
    bool analyzed = false;
    for (int it = 0; it < maxSteps; ++it) {
        carrierScales();
        updateMobilities(x);
        assemble(x, mass, history, F, &trip);
        // Dirichlet rows: x - target.
        std::vector<Eigen::Triplet<double>> kept;
        kept.reserve(trip.size() + std::size_t(a_unknowns));
        for (const auto& t : trip)
            if (!fixed[std::size_t(t.row())]) kept.emplace_back(t.row(), t.col(), t.value() * colScale(t.col()));
        for (int k = 0; k < a_unknowns; ++k)
            if (fixed[std::size_t(k)]) {
                kept.emplace_back(k, k, colScale(k));
                F(k) = x(k) - target(k);
            }
        Eigen::SparseMatrix<double> J(a_unknowns, a_unknowns);
        J.setFromTriplets(kept.begin(), kept.end());
        // Row equilibration.
        Eigen::VectorXd rowMax = Eigen::VectorXd::Zero(a_unknowns);
        for (int k = 0; k < J.outerSize(); ++k)
            for (Eigen::SparseMatrix<double>::InnerIterator itr(J, k); itr; ++itr)
                rowMax(itr.row()) = std::max(rowMax(itr.row()), std::abs(itr.value()));
        for (int k = 0; k < a_unknowns; ++k)
            if (!(rowMax(k) > 0)) rowMax(k) = 1;
        for (int k = 0; k < J.outerSize(); ++k)
            for (Eigen::SparseMatrix<double>::InnerIterator itr(J, k); itr; ++itr) itr.valueRef() /= rowMax(itr.row());
        const Eigen::VectorXd rhs = -(F.array() / rowMax.array()).matrix();
        J.makeCompressed();
        if (!analyzed) {
            lu.analyzePattern(J);
            analyzed = true;
        }
        lu.factorize(J);
        if (lu.info() != Eigen::Success) {
            *why = tr("%1: its Jacobian is singular (%2)").arg(a_node->name(), QString::fromStdString(lu.lastErrorMessage()));
            return false;
        }
        Eigen::VectorXd dx = lu.solve(rhs);
        if (!dx.allFinite()) {
            *why = tr("%1: Newton's step is not finite").arg(a_node->name());
            return false;
        }
        dx = (dx.array() * colScale.array()).matrix();
        // Damping: the potential moved no more than 10 Vt at once; the
        // carriers kept positive.
        double maxDV = 0;
        for (int i = 0; i < a_n; ++i)
            if (a_iV[std::size_t(i)] >= 0) maxDV = std::max(maxDV, std::abs(dx(a_iV[std::size_t(i)])));
        const double alpha = maxDV > 10 * a_Vt ? 10 * a_Vt / maxDV : 1.0;
        double changeV = 0, changeC = 0;
        for (int i = 0; i < a_n; ++i) {
            if (a_iV[std::size_t(i)] >= 0) {
                const int k = a_iV[std::size_t(i)];
                x(k) += alpha * dx(k);
                changeV = std::max(changeV, std::abs(alpha * dx(k)) / a_Vt);
            }
            for (int k : {a_iN[std::size_t(i)], a_iP[std::size_t(i)]}) {
                if (k < 0) continue;
                const double before = x(k);
                double after = before + alpha * dx(k);
                if (after < 0.05 * before) after = 0.05 * before;
                x(k) = after;
                changeC = std::max(changeC, std::abs(after - before) / (before + 1e-10 * a_ni[std::size_t(i)] + 1e-300));
            }
        }
        if (steps) *steps = it + 1;
        // Converged: the potential still to 1e-8 Vt and the carriers to 1e-6
        // (or the potential to 1e-12 Vt: the carriers' last digits are noise).
        if (alpha == 1.0 && changeV < 1e-8 && (changeC < 1e-6 || changeV < 1e-12)) return true;
    }
    *why = tr("%1: Newton's method did not converge in %2 steps").arg(a_node->name()).arg(maxSteps);
    return false;
}

bool SemiconductorProblem::atEquilibrium(const std::vector<double>& goal, double* fermi) const
{
    for (double g : a_generation)
        if (g != 0) return false;
    bool any = false;
    double v = 0;
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        if (a_contacts[k].kind == Contact::Gate) continue;
        if (!any) v = goal[k];
        else if (std::abs(goal[k] - v) > 1e-12) return false;
        any = true;
    }
    *fermi = v;
    return any;
}

bool SemiconductorProblem::equilibriumPoisson(Eigen::VectorXd& x, double fermi, const std::vector<double>& applied, bool warm, QString* why)
{
    // V alone; n = ni exp((V - φ)/Vt), p = ni exp((φ - V)/Vt).
    contactValues(applied);
    if (!warm)
        for (int i = 0; i < a_n; ++i) {
            if (a_iV[std::size_t(i)] < 0) continue;
            double v = fermi;
            if (a_nodeDomain[std::size_t(i)] >= 0) v += a_Vt * std::asinh(doping(i) / (2 * a_ni[std::size_t(i)]));
            x(a_iV[std::size_t(i)]) = v;
        }
    std::vector<int> vIndex;
    std::vector<int> local(std::size_t(a_unknowns), -1);
    for (int i = 0; i < a_n; ++i)
        if (a_iV[std::size_t(i)] >= 0) {
            local[std::size_t(a_iV[std::size_t(i)])] = int(vIndex.size());
            vIndex.push_back(i);
        }
    const int nv = int(vIndex.size());
    std::vector<char> fixed(std::size_t(nv), 0);
    for (const Contact& c : a_contacts)
        for (int i : c.nodes) {
            const int k = local[std::size_t(a_iV[std::size_t(i)])];
            fixed[std::size_t(k)] = 1;
            x(a_iV[std::size_t(i)]) = a_bcV[std::size_t(i)];
        }
    Eigen::SparseLU<Eigen::SparseMatrix<double>> lu;
    bool analyzed = false;
    for (int it = 0; it < 200; ++it) {
        Eigen::VectorXd Fv;
        Eigen::SparseMatrix<double> J;
        equilibriumSystem(x, fermi, local, vIndex, fixed, false, Fv, J);
        if (!analyzed) {
            lu.analyzePattern(J);
            analyzed = true;
        }
        lu.factorize(J);
        if (lu.info() != Eigen::Success) {
            *why = tr("%1: the equilibrium's potential: a singular matrix").arg(a_node->name());
            return false;
        }
        const Eigen::VectorXd dv = lu.solve(-Fv);
        double maxDV = 0;
        for (int k = 0; k < nv; ++k) maxDV = std::max(maxDV, std::abs(dv(k)));
        const double alpha = maxDV > 5 * a_Vt ? 5 * a_Vt / maxDV : 1.0;
        for (int k = 0; k < nv; ++k) x(a_iV[std::size_t(vIndex[std::size_t(k)])]) += alpha * dv(k);
        if (alpha == 1.0 && maxDV < 1e-10 * a_Vt) {
            for (int i = 0; i < a_n; ++i) {
                if (a_iN[std::size_t(i)] < 0) continue;
                const double v = (x(a_iV[std::size_t(i)]) - fermi) / a_Vt, ni = a_ni[std::size_t(i)];
                x(a_iN[std::size_t(i)]) = ni * std::exp(std::clamp(v, -600.0, 600.0));
                x(a_iP[std::size_t(i)]) = ni * std::exp(std::clamp(-v, -600.0, 600.0));
            }
            return true;
        }
    }
    *why = tr("%1: no equilibrium potential found in 200 steps").arg(a_node->name());
    return false;
}

void SemiconductorProblem::equilibriumSystem(const Eigen::VectorXd& x0, double fermi, const std::vector<int>& local, const std::vector<int>& vIndex,
                                             const std::vector<char>& fixed, bool keepContacts, Eigen::VectorXd& Fv,
                                             Eigen::SparseMatrix<double>& J) const
{
    // The carriers from V; the full assembly's Poisson rows give F and ∂F/∂V
    // (with ∂/∂n, ∂/∂p folded in through dn/dV = n/Vt, dp/dV = -p/Vt).
    Eigen::VectorXd x = x0;
    for (int i = 0; i < a_n; ++i) {
        if (a_iN[std::size_t(i)] < 0) continue;
        const double v = (x(a_iV[std::size_t(i)]) - fermi) / a_Vt, ni = a_ni[std::size_t(i)];
        x(a_iN[std::size_t(i)]) = ni * std::exp(std::clamp(v, -600.0, 600.0));
        x(a_iP[std::size_t(i)]) = ni * std::exp(std::clamp(-v, -600.0, 600.0));
    }
    Eigen::VectorXd F;
    std::vector<Eigen::Triplet<double>> trip;
    assemble(x, 0, nullptr, F, &trip);
    const int nv = int(vIndex.size());
    Fv.resize(nv);
    for (int k = 0; k < nv; ++k) Fv(k) = fixed[std::size_t(k)] && !keepContacts ? 0.0 : F(a_iV[std::size_t(vIndex[std::size_t(k)])]);
    std::vector<Eigen::Triplet<double>> kept;
    for (const auto& t : trip) {
        const int r = local[std::size_t(t.row())];
        if (r < 0 || (fixed[std::size_t(r)] && !keepContacts)) continue;
        int c = local[std::size_t(t.col())];
        double v = t.value();
        if (c < 0) {
            // ∂/∂n or ∂/∂p of this node's V row: through n(V), p(V).
            const int node = vIndex[std::size_t(r)];
            const bool isN = a_iN[std::size_t(node)] == t.col();
            if (!isN && a_iP[std::size_t(node)] != t.col()) continue;
            v *= (isN ? 1 : -1) * x(t.col()) / a_Vt;
            c = r;
        }
        kept.emplace_back(r, c, v);
    }
    if (!keepContacts)
        for (int k = 0; k < nv; ++k)
            if (fixed[std::size_t(k)]) kept.emplace_back(k, k, 1.0);
    J.resize(nv, nv);
    J.setFromTriplets(kept.begin(), kept.end());
    J.makeCompressed();
}

void SemiconductorProblem::quasiStatic(const Eigen::VectorXd& x, double fermi, const std::vector<double>& applied)
{
    a_C0.clear();
    a_gates.clear();
    for (int k = 0; k < int(a_contacts.size()); ++k)
        if (a_contacts[std::size_t(k)].kind == Contact::Gate) a_gates.push_back(k);
    if (a_gates.empty()) return;
    contactValues(applied);
    std::vector<int> vIndex;
    std::vector<int> local(std::size_t(a_unknowns), -1);
    for (int i = 0; i < a_n; ++i)
        if (a_iV[std::size_t(i)] >= 0) {
            local[std::size_t(a_iV[std::size_t(i)])] = int(vIndex.size());
            vIndex.push_back(i);
        }
    const int nv = int(vIndex.size());
    std::vector<char> fixed(std::size_t(nv), 0);
    for (const Contact& c : a_contacts)
        for (int i : c.nodes) fixed[std::size_t(local[std::size_t(a_iV[std::size_t(i)])])] = 1;
    Eigen::VectorXd Fv;
    Eigen::SparseMatrix<double> J, full;
    equilibriumSystem(x, fermi, local, vIndex, fixed, false, Fv, J);
    equilibriumSystem(x, fermi, local, vIndex, fixed, true, Fv, full);
    Eigen::SparseLU<Eigen::SparseMatrix<double>> lu;
    lu.compute(J);
    if (lu.info() != Eigen::Success) return;
    // Each gate at 1 V more, the other contacts held: each gate's charge.
    const Eigen::SparseMatrix<double, Eigen::RowMajor> rows = full;
    a_C0.assign(a_gates.size(), std::vector<double>(a_gates.size(), 0.0));
    for (std::size_t b = 0; b < a_gates.size(); ++b) {
        Eigen::VectorXd rhs = Eigen::VectorXd::Zero(nv);
        for (int i : a_contacts[std::size_t(a_gates[b])].nodes) rhs(local[std::size_t(a_iV[std::size_t(i)])]) = 1;
        const Eigen::VectorXd dv = lu.solve(rhs);
        for (std::size_t a = 0; a < a_gates.size(); ++a) {
            double dq = 0;
            for (int i : a_contacts[std::size_t(a_gates[a])].nodes) {
                const int r = local[std::size_t(a_iV[std::size_t(i)])];
                for (Eigen::SparseMatrix<double, Eigen::RowMajor>::InnerIterator it(rows, r); it; ++it) dq -= it.value() * dv(it.col());
            }
            a_C0[a][b] = dq;
        }
    }
}

Eigen::VectorXd SemiconductorProblem::stateVector() const
{
    const Field& f = a_s.fields()[std::size_t(a_fi)];
    const std::size_t N = std::size_t(a_n);
    Eigen::VectorXd x = Eigen::VectorXd::Zero(a_unknowns);
    for (int i = 0; i < a_n; ++i) {
        if (a_iV[std::size_t(i)] >= 0) x(a_iV[std::size_t(i)]) = f.values[std::size_t(i)];
        if (a_iN[std::size_t(i)] >= 0) x(a_iN[std::size_t(i)]) = f.values[N + std::size_t(i)];
        if (a_iP[std::size_t(i)] >= 0) x(a_iP[std::size_t(i)]) = f.values[2 * N + std::size_t(i)];
    }
    return x;
}

void SemiconductorProblem::store(const Eigen::VectorXd& x)
{
    Field& f = field();
    const std::size_t N = std::size_t(a_n);
    for (int i = 0; i < a_n; ++i) {
        f.values[std::size_t(i)] = a_iV[std::size_t(i)] >= 0 ? x(a_iV[std::size_t(i)]) : 0.0;
        f.values[N + std::size_t(i)] = a_iN[std::size_t(i)] >= 0 ? x(a_iN[std::size_t(i)]) : 0.0;
        f.values[2 * N + std::size_t(i)] = a_iP[std::size_t(i)] >= 0 ? x(a_iP[std::size_t(i)]) : 0.0;
    }
    f.solved = true;
    for (std::size_t k = 0; k < a_contacts.size() && k < f.terminals.size(); ++k) {
        f.terminals[k].potential = a_contacts[k].applied;
        f.terminals[k].charge = a_contacts[k].current;
        f.terminals[k].chargeIm = a_contacts[k].charge;
    }
}

std::vector<std::vector<double>> SemiconductorProblem::currentWeights(const Eigen::VectorXd& x)
{
    const std::size_t nc = a_contacts.size();
    std::vector<std::vector<double>> weight(nc);
    // Held: each node of a contact that holds carriers (ohmic, Schottky);
    // the semiconductor's other nodes free.
    std::vector<int> held(std::size_t(a_n), -1), local(std::size_t(a_n), -1);
    std::vector<int> columns(nc, -1);
    int carriersHeld = 0;
    for (std::size_t k = 0; k < nc; ++k) {
        if (a_contacts[k].kind == Contact::Gate) continue;
        columns[k] = carriersHeld++;
        for (int i : a_contacts[k].nodes) held[std::size_t(i)] = int(k);
    }
    int free = 0;
    for (int i = 0; i < a_n; ++i)
        if (a_nodeDomain[std::size_t(i)] >= 0 && held[std::size_t(i)] < 0) local[std::size_t(i)] = free++;
    // Each edge's conductance: its currents' change with the quasi-Fermi
    // potentials' difference (n e^(..) B weighed), relative to the largest
    // and at least 10^-12 of it (a junction's depletion has less: the
    // weight's step is there all the same).
    std::vector<double> g(a_edges.size(), 0.0);
    double largest = 0;
    for (std::size_t k = 0; k < a_edges.size(); ++k) {
        const Edge& e = a_edges[k];
        if (e.w <= 0) continue;
        const int i = e.i, j = e.j;
        const double d = (x(a_iV[std::size_t(j)]) - x(a_iV[std::size_t(i)])) / a_Vt;
        const double bp = bern(d), bm = bern(-d);
        const double ni = x(a_iN[std::size_t(i)]), nj = x(a_iN[std::size_t(j)]), pi = x(a_iP[std::size_t(i)]), pj = x(a_iP[std::size_t(j)]);
        g[k] = e.wn * 0.5 * (ni * bm + nj * bp) + e.wp * 0.5 * (pi * bp + pj * bm);
        if (std::isfinite(g[k])) largest = std::max(largest, g[k]);
    }
    for (double& v : g) v = largest > 0 && std::isfinite(v) ? std::max(v / largest, 1e-12) : 1e-12;
    Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(free, std::max(carriersHeld, 1));
    if (free > 0 && carriersHeld > 0) {
        std::vector<Eigen::Triplet<double>> trip;
        trip.reserve(4 * a_edges.size() + std::size_t(free));
        for (std::size_t k = 0; k < a_edges.size(); ++k) {
            const int i = a_edges[k].i, j = a_edges[k].j;
            const int li = local[std::size_t(i)], lj = local[std::size_t(j)];
            const double gk = g[k];
            if (li >= 0) trip.emplace_back(li, li, gk);
            if (lj >= 0) trip.emplace_back(lj, lj, gk);
            if (li >= 0 && lj >= 0) {
                trip.emplace_back(li, lj, -gk);
                trip.emplace_back(lj, li, -gk);
            } else if (li >= 0 && held[std::size_t(j)] >= 0) {
                rhs(li, columns[std::size_t(held[std::size_t(j)])]) += gk;
            } else if (lj >= 0 && held[std::size_t(i)] >= 0) {
                rhs(lj, columns[std::size_t(held[std::size_t(i)])]) += gk;
            }
        }
        // (A little to nought everywhere: a region no contact reaches is
        // weighed nought, not singular.)
        for (int k = 0; k < free; ++k) trip.emplace_back(k, k, 1e-14);
        Eigen::SparseMatrix<double> L(free, free);
        L.setFromTriplets(trip.begin(), trip.end());
        if (a_weightSize != free) {
            a_weightSolver.analyzePattern(L);
            a_weightSize = free;
        }
        a_weightSolver.factorize(L);
        if (a_weightSolver.info() == Eigen::Success) rhs = a_weightSolver.solve(rhs);
        else rhs.setZero();
    }
    for (std::size_t k = 0; k < nc; ++k) {
        if (columns[k] < 0) continue;
        std::vector<double>& w = weight[k];
        w.assign(std::size_t(a_n), 0.0);
        for (int i = 0; i < a_n; ++i) {
            if (held[std::size_t(i)] >= 0) w[std::size_t(i)] = held[std::size_t(i)] == int(k) ? 1.0 : 0.0;
            else if (local[std::size_t(i)] >= 0) w[std::size_t(i)] = std::clamp(rhs(local[std::size_t(i)], columns[k]), 0.0, 1.0);
        }
    }
    return weight;
}

void SemiconductorProblem::terminalValues(const Eigen::VectorXd& x, double mass, const std::vector<double>* history, bool still)
{
    Eigen::VectorXd F;
    assemble(x, mass, history, F, nullptr);
    for (Contact& c : a_contacts) {
        double Qc = 0;
        for (int i : c.nodes) Qc -= F(a_iV[std::size_t(i)]);
        c.charge = Qc;
        c.current = 0;
    }
    // At equilibrium no current flows: nought, not the rounding of the
    // contacts' fluxes (-291 pA on a 10^19 cm^-3 one).
    if (still) return;
    // Each contact's: Σ over the edges of their currents times its weight's
    // change, and the carriers' change in time where it weighs (the
    // recombination's share is the electrons' and the holes' alike: none).
    const std::vector<std::vector<double>> weight = currentWeights(x);
    const std::size_t N = std::size_t(a_n);
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        const std::vector<double>& w = weight[k];
        if (w.empty()) continue;
        double I = 0;
        for (const Edge& e : a_edges) {
            if (e.w <= 0) continue;
            const double dw = w[std::size_t(e.i)] - w[std::size_t(e.j)];
            if (dw == 0) continue;
            const int i = e.i, j = e.j;
            const double d = (x(a_iV[std::size_t(j)]) - x(a_iV[std::size_t(i)])) / a_Vt;
            const double bp = bern(d), bm = bern(-d);
            const double fn = e.wn * (x(a_iN[std::size_t(j)]) * bp - x(a_iN[std::size_t(i)]) * bm);
            const double fp = e.wp * (x(a_iP[std::size_t(i)]) * bp - x(a_iP[std::size_t(j)]) * bm);
            I += (fn + fp) * dw;
        }
        if (mass > 0 && history)
            for (int i = 0; i < a_n; ++i) {
                if (w[std::size_t(i)] == 0 || a_iN[std::size_t(i)] < 0) continue;
                const double A = a_box[std::size_t(i)];
                const double dn = mass * x(a_iN[std::size_t(i)]) - (*history)[N + std::size_t(i)];
                const double dp = mass * x(a_iP[std::size_t(i)]) - (*history)[2 * N + std::size_t(i)];
                I += w[std::size_t(i)] * Q * A * (dp - dn);
            }
        a_contacts[k].current = I;
    }
}

bool SemiconductorProblem::stationary(QString* why)
{
    Field& f = field();
    std::vector<double> goal;
    if (!targets(goal, why)) return false;
    Eigen::VectorXd x = stateVector();
    auto finish = [&](const Eigen::VectorXd& solved, bool still) {
        for (std::size_t k = 0; k < a_contacts.size(); ++k) a_contacts[k].applied = goal[k];
        terminalValues(solved, 0, nullptr, still);
        store(solved);
    };
    double fermi = 0;
    if (atEquilibrium(goal, &fermi)) {
        // No current: Poisson's equation alone (a MOS capacitor, a junction
        // unbiased) - exact, the currents' equations hold at once.
        if (!equilibriumPoisson(x, fermi, goal, f.solved, why)) return false;
        finish(x, true);
        quasiStatic(x, fermi, goal);
        return true;
    }
    a_C0.clear();
    std::vector<double> at(a_contacts.size(), 0.0);
    if (!f.solved) {
        if (!equilibriumPoisson(x, 0, at, false, why)) return false;
        for (Contact& c : a_contacts) c.applied = 0;
    } else {
        for (std::size_t k = 0; k < a_contacts.size(); ++k) at[k] = a_contacts[k].applied;
    }
    // From the voltages it is at to those asked, in steps that Newton's
    // method takes (smaller when it does not).
    double step = 0.1;
    int solves = 0;
    QString failed;
    while (true) {
        double left = 0;
        for (std::size_t k = 0; k < goal.size(); ++k) left = std::max(left, std::abs(goal[k] - at[k]));
        if (left < 1e-12 && solves > 0) break;
        const double frac = left > 0 ? std::min(1.0, step / left) : 1.0;
        std::vector<double> next(at.size());
        for (std::size_t k = 0; k < at.size(); ++k) next[k] = at[k] + frac * (goal[k] - at[k]);
        contactValues(next);
        Eigen::VectorXd trial = x;
        int steps = 0;
        if (newton(trial, 0, nullptr, 40, &steps, &failed)) {
            x = trial;
            at = next;
            ++solves;
            if (steps < 8) step = std::min(step * 2, 1.0);
        } else {
            step /= 4;
            if (step < 1e-5) {
                // (Not "did not converge": a step at rest that fails is an error.)
                *why = tr("%1: no solution found on the way to the contacts' voltages (steps down to 10 µV): %2")
                           .arg(a_node->name(), failed.section(QLatin1String(": "), 1));
                return false;
            }
        }
    }
    finish(x, false);
    if (solves > 1) a_s.log << tr("%1: the contacts' voltages reached in %2 steps").arg(a_node->name()).arg(solves);
    return true;
}

bool SemiconductorProblem::smallSignal(double frequency, QString* why)
{
    const Eigen::VectorXd x = stateVector();
    std::vector<double> at(a_contacts.size());
    for (std::size_t k = 0; k < a_contacts.size(); ++k) at[k] = a_contacts[k].applied;
    contactValues(at);
    updateMobilities(x);
    Eigen::VectorXd F;
    std::vector<Eigen::Triplet<double>> trip;
    assemble(x, 0, nullptr, F, &trip);
    std::vector<char> fixed(std::size_t(a_unknowns), 0);
    for (const Contact& c : a_contacts)
        for (int i : c.nodes) {
            fixed[std::size_t(a_iV[std::size_t(i)])] = 1;
            if (c.kind == Contact::Ohmic) fixed[std::size_t(a_iN[std::size_t(i)])] = fixed[std::size_t(a_iP[std::size_t(i)])] = 1;
        }
    // J + jω M (M the carriers' boxes: -qA on n's rows, +qA on p's), solved in
    // the quasi-Fermi potentials' changes: δn = n/Vt (δV - δφn), δp = p/Vt
    // (δφp - δV). In n and p a current near nought is the difference of two
    // large terms (a MOS capacitor's inversion electrons, 10^14 above the
    // bulk's): in φ its gradient, well conditioned.
    const double omega = 2 * M_PI * frequency;
    using cd = std::complex<double>;
    std::vector<int> colNode(std::size_t(a_unknowns), -1), colKind(std::size_t(a_unknowns), 0);   // 0 V, 1 n, 2 p
    for (int i = 0; i < a_n; ++i) {
        if (a_iV[std::size_t(i)] >= 0) colNode[std::size_t(a_iV[std::size_t(i)])] = i;
        if (a_iN[std::size_t(i)] >= 0) {
            colNode[std::size_t(a_iN[std::size_t(i)])] = i;
            colKind[std::size_t(a_iN[std::size_t(i)])] = 1;
            colNode[std::size_t(a_iP[std::size_t(i)])] = i;
            colKind[std::size_t(a_iP[std::size_t(i)])] = 2;
        }
    }
    std::vector<Eigen::Triplet<cd>> ct;
    // An entry of the n-p system in the φ one's columns (each in units of Vt).
    auto put = [&](int r, int c, cd v) {
        const int i = colNode[std::size_t(c)];
        if (colKind[std::size_t(c)] == 0) ct.emplace_back(r, c, v * a_Vt);
        else if (colKind[std::size_t(c)] == 1) {
            const double s = x(c) / a_Vt * a_Vt;   // n/Vt, times Vt
            ct.emplace_back(r, a_iV[std::size_t(i)], v * s);
            ct.emplace_back(r, c, -v * s);
        } else {
            const double s = x(c) / a_Vt * a_Vt;
            ct.emplace_back(r, a_iV[std::size_t(i)], -v * s);
            ct.emplace_back(r, c, v * s);
        }
    };
    for (const auto& t : trip)
        if (!fixed[std::size_t(t.row())]) put(t.row(), t.col(), cd(t.value(), 0));
    for (int i = 0; i < a_n; ++i) {
        const double A = a_box[std::size_t(i)];
        const int rn = a_iN[std::size_t(i)], rp = a_iP[std::size_t(i)];
        if (rn >= 0 && !fixed[std::size_t(rn)]) put(rn, rn, cd(0, -omega * Q * A));
        if (rp >= 0 && !fixed[std::size_t(rp)]) put(rp, rp, cd(0, omega * Q * A));
    }
    // Fixed: δV at a contact; δφn = δφp = δVa at an ohmic one (its carriers
    // fixed).
    for (int k = 0; k < a_unknowns; ++k)
        if (fixed[std::size_t(k)]) ct.emplace_back(k, k, cd(a_Vt, 0));
    Eigen::SparseMatrix<cd> A(a_unknowns, a_unknowns);
    A.setFromTriplets(ct.begin(), ct.end());
    Eigen::VectorXd rowMax = Eigen::VectorXd::Zero(a_unknowns);
    for (int k = 0; k < A.outerSize(); ++k)
        for (Eigen::SparseMatrix<cd>::InnerIterator itr(A, k); itr; ++itr) rowMax(itr.row()) = std::max(rowMax(itr.row()), std::abs(itr.value()));
    for (int k = 0; k < a_unknowns; ++k)
        if (!(rowMax(k) > 0)) rowMax(k) = 1;
    for (int k = 0; k < A.outerSize(); ++k)
        for (Eigen::SparseMatrix<cd>::InnerIterator itr(A, k); itr; ++itr) itr.valueRef() /= rowMax(itr.row());
    A.makeCompressed();
    Eigen::SparseLU<Eigen::SparseMatrix<cd>> lu;
    lu.analyzePattern(A);
    lu.factorize(A);
    if (lu.info() != Eigen::Success) {
        *why = tr("%1: the small-signal matrix is singular").arg(a_node->name());
        return false;
    }
    // The reactions' rows (each contact node's V row as assembled: its
    // charge); the currents weighed as at rest (currentWeights()).
    std::map<int, std::vector<std::pair<int, double>>> rows;
    for (const auto& t : trip)
        if (fixed[std::size_t(t.row())] && colNode[std::size_t(t.row())] >= 0 && colKind[std::size_t(t.row())] == 0) rows[t.row()].emplace_back(t.col(), t.value());
    const std::size_t nc = a_contacts.size();
    const std::vector<std::vector<double>> weight = currentWeights(x);
    a_Y.assign(nc, std::vector<cd>(nc, cd(0, 0)));
    for (std::size_t k = 0; k < nc; ++k) {
        Eigen::VectorXcd rhs = Eigen::VectorXcd::Zero(a_unknowns);
        // (A fixed row reads Vt y = 1 V: y in units of Vt.)
        for (int i : a_contacts[k].nodes) {
            rhs(a_iV[std::size_t(i)]) = cd(1.0 / rowMax(a_iV[std::size_t(i)]), 0);
            if (a_contacts[k].kind == Contact::Ohmic) {
                rhs(a_iN[std::size_t(i)]) = cd(1.0 / rowMax(a_iN[std::size_t(i)]), 0);
                rhs(a_iP[std::size_t(i)]) = cd(1.0 / rowMax(a_iP[std::size_t(i)]), 0);
            }
        }
        const Eigen::VectorXcd dy = lu.solve(rhs);   // δV/Vt, δφn/Vt, δφp/Vt
        Eigen::VectorXcd dx(a_unknowns);
        for (int j = 0; j < a_unknowns; ++j) {
            const int i = colNode[std::size_t(j)];
            if (colKind[std::size_t(j)] == 0) dx(j) = dy(j) * a_Vt;
            else if (colKind[std::size_t(j)] == 1) dx(j) = x(j) * (dy(a_iV[std::size_t(i)]) - dy(j));
            else dx(j) = x(j) * (dy(j) - dy(a_iV[std::size_t(i)]));
        }
        auto rowDot = [&](int r) {
            cd s(0, 0);
            auto it = rows.find(r);
            if (it == rows.end()) return s;
            for (const auto& [c, v] : it->second) s += v * dx(c);
            return s;
        };
        // Each edge's change of current (Scharfetter and Gummel's, linear).
        std::vector<cd> df(a_edges.size(), cd(0, 0));
        for (std::size_t m = 0; m < a_edges.size(); ++m) {
            const Edge& e = a_edges[m];
            if (e.w <= 0) continue;
            const int i = e.i, j = e.j;
            const double d = (x(a_iV[std::size_t(j)]) - x(a_iV[std::size_t(i)])) / a_Vt;
            const double bp = bern(d), bm = bern(-d), dbp = dbern(d), dbm = dbern(-d);
            const int ni = a_iN[std::size_t(i)], nj = a_iN[std::size_t(j)], pi = a_iP[std::size_t(i)], pj = a_iP[std::size_t(j)];
            const cd dV = (dx(a_iV[std::size_t(j)]) - dx(a_iV[std::size_t(i)])) / a_Vt;
            const cd dfn = e.wn * (bp * dx(nj) - bm * dx(ni) + (x(nj) * dbp + x(ni) * dbm) * dV);
            const cd dfp = e.wp * (bp * dx(pi) - bm * dx(pj) + (x(pi) * dbp + x(pj) * dbm) * dV);
            df[m] = dfn + dfp;
        }
        for (std::size_t c = 0; c < nc; ++c) {
            const Contact& ct2 = a_contacts[c];
            cd dI(0, 0), dQ(0, 0);
            for (int i : ct2.nodes) dQ -= rowDot(a_iV[std::size_t(i)]);
            const std::vector<double>& w = weight[c];
            if (!w.empty()) {
                for (std::size_t m = 0; m < a_edges.size(); ++m) {
                    const double dw = w[std::size_t(a_edges[m].i)] - w[std::size_t(a_edges[m].j)];
                    if (dw != 0) dI += df[m] * dw;
                }
                // (The carriers' change, -qA δn and qA δp on their rows, at jω.)
                for (int i = 0; i < a_n; ++i)
                    if (w[std::size_t(i)] != 0 && a_iN[std::size_t(i)] >= 0)
                        dI += w[std::size_t(i)] * cd(0, omega) * Q * a_box[std::size_t(i)] * (dx(a_iP[std::size_t(i)]) - dx(a_iN[std::size_t(i)]));
            }
            a_Y[c][k] = dI + cd(0, omega) * dQ;
        }
    }
    a_frequency = frequency;
    return true;
}

void SemiconductorProblem::initialize()
{
    // From where it is (a step before's), or a solution at rest at the
    // contacts' voltages now: a study in time starts there.
    if (field().solved) return;
    a_generation.assign(std::size_t(a_n), 0.0);
    QString why;
    if (!stationary(&why)) a_initError = why;
}

bool SemiconductorProblem::solve(const SolveOptions& o, QString* error)
{
    if (!a_initError.isEmpty()) {
        *error = a_initError;
        return false;
    }
    // Generation at the nodes, as it is now (it may read other fields, t).
    a_generation.assign(std::size_t(a_n), 0.0);
    {
        PointEvaluator ev(a_s);
        for (int i = 0; i < a_n; ++i) {
            const int d = a_nodeDomain[std::size_t(i)];
            if (d < 0 || a_media[std::size_t(d)].generation.empty()) continue;
            const auto& [t, r] = a_nodePoint[std::size_t(i)];
            ev.moveTo(t, r[0], r[1]);
            double g = 0;
            for (const Expression& e : a_media[std::size_t(d)].generation) {
                const double v = ev.eval(e);
                if (std::isfinite(v)) g += v;
            }
            a_generation[std::size_t(i)] = g;
        }
    }
    if (o.harmonic && o.frequency > 0) {
        if (!field().solved && !stationary(error)) return false;
        return smallSignal(o.frequency, error);
    }
    a_Y.clear();
    if (o.mass > 0 && o.history) {
        // A step in time: straight to the contacts' voltages now.
        std::vector<double> goal;
        if (!targets(goal, error)) return false;
        contactValues(goal);
        Eigen::VectorXd x = stateVector();
        int steps = 0;
        QString why;
        if (!newton(x, o.mass, o.history, 40, &steps, &why)) {
            *error = why;   // ("did not converge": the step is taken again, smaller)
            return false;
        }
        for (std::size_t k = 0; k < a_contacts.size(); ++k) a_contacts[k].applied = goal[k];
        terminalValues(x, o.mass, o.history);
        // (Displacement current: the contact's charge's change.)
        {
            // Q at the step's start from the history's charge: the BDF
            // formula applied to Q (affine in the unknowns) is mass Q(x) - Q(history).
            std::vector<double> hv = *o.history;
            Eigen::VectorXd xh = Eigen::VectorXd::Zero(a_unknowns);
            const std::size_t N = std::size_t(a_n);
            for (int i = 0; i < a_n; ++i) {
                if (a_iV[std::size_t(i)] >= 0) xh(a_iV[std::size_t(i)]) = hv[std::size_t(i)];
                if (a_iN[std::size_t(i)] >= 0) xh(a_iN[std::size_t(i)]) = hv[N + std::size_t(i)];
                if (a_iP[std::size_t(i)] >= 0) xh(a_iP[std::size_t(i)]) = hv[2 * N + std::size_t(i)];
            }
            // The charges' linear parts: Poisson's rows less the doping's.
            auto charges = [&](const Eigen::VectorXd& v, bool withDoping) {
                Eigen::VectorXd F;
                assemble(v, 0, nullptr, F, nullptr);
                std::vector<double> q(a_contacts.size(), 0);
                for (std::size_t k = 0; k < a_contacts.size(); ++k)
                    for (int i : a_contacts[k].nodes) {
                        double fv = F(a_iV[std::size_t(i)]);
                        if (!withDoping && a_nodeDomain[std::size_t(i)] >= 0) fv -= Q * a_box[std::size_t(i)] * doping(i);
                        q[k] -= fv;
                    }
                return q;
            };
            const std::vector<double> now = charges(x, false), before = charges(xh, false);
            for (std::size_t k = 0; k < a_contacts.size(); ++k) a_contacts[k].current += o.mass * now[k] - before[k];
        }
        store(x);
        return true;
    }
    return stationary(error);
}

QString matrixEntryName(const QString& tag, const QString& letter, const QString& a, const QString& b)
{
    if (a.size() == 1 && b.size() == 1) return tag + QLatin1Char('.') + letter + a + b;
    return tag + QLatin1Char('.') + letter + QLatin1Char('_') + a + QLatin1Char('_') + b;
}

void SemiconductorProblem::setGlobals()
{
    const QString p = field().tag + QLatin1Char('.');
    for (const Contact& c : a_contacts) {
        a_s.setGlobal(p + QStringLiteral("I_") + c.name, c.current, Dim::current(), tr("current into the device at %1").arg(c.name));
        a_s.setGlobal(p + QStringLiteral("V_") + c.name, c.applied, Dim::voltage(), tr("voltage applied at %1").arg(c.name));
        // (An ohmic contact on neutral silicon holds almost none: the
        // junction's charge is balanced inside it.)
        a_s.setGlobal(p + QStringLiteral("Q_") + c.name, c.charge, Dim::charge(),
                      c.kind == Contact::Ohmic ? tr("charge on %1's metal - about nought on neutral silicon (a junction's charge is balanced "
                                                    "inside it); its capacitance is the small-signal %2")
                                                     .arg(c.name, matrixEntryName(field().tag, QStringLiteral("C"), c.name, c.name))
                                               : tr("charge on %1's metal").arg(c.name));
    }
    for (std::size_t a = 0; a < a_C0.size(); ++a)
        for (std::size_t b = 0; b < a_C0.size(); ++b) {
            const QString na = a_contacts[std::size_t(a_gates[a])].name, nb = a_contacts[std::size_t(a_gates[b])].name;
            a_s.setGlobal(matrixEntryName(field().tag, QStringLiteral("C0"), na, nb), a_C0[a][b], Dim::capacitance(),
                          tr("quasi-static capacitance at equilibrium (the carriers following: low frequency), %1 from %2").arg(na, nb));
        }
    if (!a_Y.empty() && a_frequency > 0) {
        const double omega = 2 * M_PI * a_frequency;
        for (std::size_t a = 0; a < a_contacts.size(); ++a)
            for (std::size_t b = 0; b < a_contacts.size(); ++b) {
                const QString na = a_contacts[a].name, nb = a_contacts[b].name;
                a_s.setGlobal(matrixEntryName(field().tag, QStringLiteral("G"), na, nb), a_Y[a][b].real(), Dim::conductance(),
                              tr("small-signal conductance, %1 from %2").arg(na, nb));
                a_s.setGlobal(matrixEntryName(field().tag, QStringLiteral("C"), na, nb), a_Y[a][b].imag() / omega, Dim::capacitance(),
                              tr("small-signal capacitance, %1 from %2").arg(na, nb));
            }
    }
}

} // namespace

std::unique_ptr<Problem> makeSemiconductorProblem(Solution& solution, int field)
{
    return std::make_unique<SemiconductorProblem>(solution, field);
}

SemiconductorState semiconductorAt(PointEvaluator& e, int f)
{
    SemiconductorState s;
    const Solution& sol = e.solution();
    const Field& field = sol.fields()[std::size_t(f)];
    const int t = e.triangle();
    if (t < 0 || field.extra.size() < std::size_t(2 + 11 * (t + 1))) return s;
    const double* ex = &field.extra[std::size_t(2 + 11 * t)];
    const double Vt = field.extra[0];
    const int d = sol.mesh().domain[std::size_t(t)];
    if (d < 0 || !field.domains[std::size_t(d)]) return s;
    const Space& sp = *field.space;
    const int* dof = sp.dofs(t);
    const std::size_t N = std::size_t(sp.size());
    const double xi = e.xi(), eta = e.eta();
    const double w[3] = {1 - xi - eta, xi, eta};
    double V[3], n[3], p[3];
    for (int k = 0; k < 3; ++k) {
        V[k] = field.values[std::size_t(dof[k])];
        n[k] = field.values[N + std::size_t(dof[k])];
        p[k] = field.values[2 * N + std::size_t(dof[k])];
    }
    s.V = w[0] * V[0] + w[1] * V[1] + w[2] * V[2];
    s.defined = true;
    if (!std::isfinite(ex[0])) return s;   // an insulator
    s.semiconductor = true;
    s.Vt = Vt;
    // Carriers interpolated in their logarithm: they vary exponentially.
    double ln = 0, lp = 0;
    for (int k = 0; k < 3; ++k) {
        ln += w[k] * std::log(std::max(n[k], 1e-300));
        lp += w[k] * std::log(std::max(p[k], 1e-300));
    }
    s.n = std::exp(ln);
    s.p = std::exp(lp);
    s.mun = ex[0];
    s.mup = ex[1];
    s.dEc = ex[2];
    s.Eg = ex[3];
    s.ni = ex[4];
    const double taun = ex[5], taup = ex[6], Et = ex[7], Cn = ex[8], Cp = ex[9], B = ex[10];
    const double excess = s.n * s.p - s.ni * s.ni;
    if (std::isfinite(taun) && std::isfinite(taup)) {
        const double kT = Vt * Q;
        const double n1 = s.ni * std::exp(Et / kT), p1 = s.ni * std::exp(-Et / kT);
        s.R += excess / (taup * (s.n + n1) + taun * (s.p + p1));
    }
    s.R += (Cn * s.n + Cp * s.p) * excess + B * excess;
    // The currents: each edge's Scharfetter-Gummel current, the vector that
    // best agrees with the three (least squares).
    const Mesh& mesh = sol.mesh();
    QPointF x[3];
    for (int k = 0; k < 3; ++k) x[k] = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * mesh.unitScale;
    double a11 = 0, a12 = 0, a22 = 0, bn[2] = {0, 0}, bp[2] = {0, 0};
    for (int k = 0; k < 3; ++k) {
        const int i = k, j = (k + 1) % 3;
        const double hx = x[j].x() - x[i].x(), hy = x[j].y() - x[i].y(), h = std::hypot(hx, hy);
        if (!(h > 0)) continue;
        const double ux = hx / h, uy = hy / h;
        const double dd = (V[j] - V[i]) / Vt;
        const double jn = Q * s.mun * Vt / h * (n[j] * bern(dd) - n[i] * bern(-dd));
        const double jp = Q * s.mup * Vt / h * (p[i] * bern(dd) - p[j] * bern(-dd));
        a11 += ux * ux;
        a12 += ux * uy;
        a22 += uy * uy;
        bn[0] += ux * jn;
        bn[1] += uy * jn;
        bp[0] += ux * jp;
        bp[1] += uy * jp;
    }
    const double det = a11 * a22 - a12 * a12;
    if (std::abs(det) > 1e-30) {
        s.Jn[0] = (a22 * bn[0] - a12 * bn[1]) / det;
        s.Jn[1] = (a11 * bn[1] - a12 * bn[0]) / det;
        s.Jp[0] = (a22 * bp[0] - a12 * bp[1]) / det;
        s.Jp[1] = (a11 * bp[1] - a12 * bp[0]) / det;
    }
    return s;
}

} // namespace qucs_s::fem::detail
