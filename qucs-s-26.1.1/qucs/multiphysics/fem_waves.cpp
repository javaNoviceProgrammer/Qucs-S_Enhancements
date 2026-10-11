/*
 * fem_waves.cpp - electromagnetic waves: a cross-section's modes (the
 *                 fields e^{-jβz} along it, full-vector: Et on Nédélec's
 *                 edge elements, Ez on Lagrange's - Lee, Sun and Cendes'
 *                 formulation, A x = -β² B x, solved by a shift and invert
 *                 about the effective index guessed) and waves in the plane
 *                 (Ez or Hz: -∇·(P ∇u) - k0² q u = 0) with ports and their
 *                 S-parameters, scattering boundaries; in media of a complex
 *                 refractive index or permittivity, anisotropic (diagonal),
 *                 warmed (dn/dT), and perfectly matched layers
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_edge.h"
#include "fem_problem.h"

#include <Eigen/Dense>

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <map>

namespace qucs_s::fem::detail {

namespace {

using cd = std::complex<double>;
constexpr double C0 = 299792458.0;
constexpr double Mu0 = 1.25663706127e-6;
const cd J(0, 1);

/// The media of a waves physics, by domain: what makes ε (complex, a
/// diagonal tensor) and µ (real) at a point - a refractive index n - jκ (n
/// along x, y and z when anisotropic; warmed: + dn/dT (T - Tref)), or a
/// permittivity with its loss tangent and conductivity - and the perfectly
/// matched layers' stretching.
class Media
{
public:
    struct Domain {
        bool defined = false;
        bool byIndex = true;
        Expression n[3], kappa, eps[3], tand, sigma, mur;
        Expression dndT, T, Tref;   // warmed: all three
        Expression carriersN, carriersP;   // the plasma dispersion's (1/m³)
        bool pml = false;
        // A PML's stretching: along x where it is left or right of the inner
        // region [x0, x1], growing to its own edge there; along y likewise
        // (a frame round the region: both at its corners).
        double inner[4] = {NaN, NaN, NaN, NaN};   // x0, x1, y0, y1 (m)
        double outer[4] = {NaN, NaN, NaN, NaN};
    };
    std::vector<Domain> domains;

    /// ε (each axis) and µ at the point the evaluator is at, at angular
    /// frequency \a omega; \a k0 for the PML's stretching.
    struct At {
        cd eps[3] = {1, 1, 1};
        cd mu[3] = {1, 1, 1};
        bool ok = false;
    };
    At at(PointEvaluator& e, int d, double omega, double k0) const
    {
        At out;
        if (d < 0 || d >= int(domains.size()) || !domains[std::size_t(d)].defined) return out;
        const Domain& m = domains[std::size_t(d)];
        auto value = [&](const Expression& x, double fallback) {
            if (!x.isValid()) return fallback;
            const double v = x.isConstant() ? x.constant() : e.eval(x);
            return std::isfinite(v) ? v : fallback;
        };
        const double mur = value(m.mur, 1.0);
        double warm = 0;
        if (m.dndT.isValid() && m.T.isValid()) warm = value(m.dndT, 0) * (value(m.T, 0) - value(m.Tref, 293.15));
        // Free carriers (silicon): Soref and Bennett's at 1550 nm, as λ² (Drude's).
        double plasma = 0, plasmaKappa = 0;
        if (m.carriersN.isValid() || m.carriersP.isValid()) {
            const double lambda = omega > 0 ? 2 * M_PI * 299792458.0 / omega : 1.55e-6;
            const double scale = (lambda / 1.55e-6) * (lambda / 1.55e-6);
            const double N = std::max(0.0, value(m.carriersN, 0)) * 1e-6, P = std::max(0.0, value(m.carriersP, 0)) * 1e-6;   // (cm⁻³)
            plasma = -(8.8e-22 * N + 8.5e-18 * std::pow(P, 0.8)) * scale;
            const double alpha = (8.5e-18 * N + 6.0e-18 * P) * scale * 100;   // (1/m)
            plasmaKappa = alpha * lambda / (4 * M_PI);
        }
        for (int k = 0; k < 3; ++k) {
            if (m.byIndex) {
                const cd n(value(m.n[k].isValid() ? m.n[k] : m.n[0], 1.0) + warm + plasma, -value(m.kappa, 0.0) - plasmaKappa);
                out.eps[k] = n * n;
            } else {
                const double er = value(m.eps[k].isValid() ? m.eps[k] : m.eps[0], 1.0);
                out.eps[k] = cd(er, -er * value(m.tand, 0.0));
                const double sigma = value(m.sigma, 0.0);
                if (sigma != 0 && omega > 0) out.eps[k] -= J * sigma / (omega * 8.8541878188e-12);
            }
            out.mu[k] = mur;
        }
        if (m.pml) {
            // Stretched coordinates: x' = ∫ sx, its ε and µ diag(sy/sx, sx/sy,
            // sx sy) times the medium's.
            const QPointF p = e.point();
            auto stretch = [&](double v, int axis) -> cd {
                const double lo = m.inner[2 * axis], hi = m.inner[2 * axis + 1];
                const double outLo = m.outer[2 * axis], outHi = m.outer[2 * axis + 1];
                double depth = 0, xi = 0;
                if (v > hi && outHi > hi) {
                    depth = outHi - hi;
                    xi = (v - hi) / depth;
                } else if (v < lo && outLo < lo) {
                    depth = lo - outLo;
                    xi = (lo - v) / depth;
                } else
                    return 1.0;
                xi = std::clamp(xi, 0.0, 1.0);
                const double kk = k0 * std::sqrt(std::max(1.0, std::abs(out.eps[2].real() * out.mu[2].real())));
                // A reflection of e^-13.8 at its outside: 2 k σ0 d / 3 = 13.8.
                const double sigma0 = 20.7 / (kk * depth);
                return cd(1.0, -sigma0 * xi * xi);
            };
            const cd sx = stretch(p.x(), 0), sy = stretch(p.y(), 1);
            const cd f[3] = {sy / sx, sx / sy, sx * sy};
            for (int k = 0; k < 3; ++k) {
                out.eps[k] *= f[k];
                out.mu[k] *= f[k];
            }
        }
        out.ok = true;
        return out;
    }

    /// Set up from \a node's features (a wave equation's, PMLs) on its
    /// domains; what is wrong to \a problem.
    void setup(Problem& owner, Solution& s, const Node& node, const std::vector<char>& on, const std::vector<const Node*>& material,
               const std::function<Expression(const QString&, const Node&, const QString&)>& compile,
               const std::function<QString(const Node&, const QString&)>& property, const std::function<void(const QString&)>& problem)
    {
        (void)owner;
        const Topology& topo = s.topology();
        const std::size_t nd = topo.domains.size();
        domains.assign(nd, Domain());
        std::vector<const Node*> modelOf(nd, nullptr);
        std::vector<char> pml(nd, 0);
        QString why;
        for (const Node& feature : node.children) {
            if (!feature.enabled) continue;
            const NodeKind* k = feature.kind();
            if (!k || k->selection != Level::Domain) continue;
            const QVector<int> picked = resolveSelection(topo, s.model(), Level::Domain, feature.selection(), &why);
            if (!why.isEmpty()) problem(QStringLiteral("%1: %2").arg(feature.name(), why));
            if (feature.type.endsWith(QLatin1String("/wave")))
                for (int d : picked) modelOf[std::size_t(d)] = &feature;
            else if (feature.type.endsWith(QLatin1String("/pml")))
                for (int d : picked) pml[std::size_t(d)] = 1;
        }
        // The region the PMLs surround: the other domains' bounds.
        QRectF inner;
        for (std::size_t d = 0; d < nd; ++d)
            if (on[d] && !pml[d]) inner = inner.isNull() ? topo.domains[d].bounds : inner.united(topo.domains[d].bounds);
        for (std::size_t d = 0; d < nd; ++d) {
            if (!on[d]) continue;
            Domain& m = domains[d];
            const Node* feat = modelOf[d];
            if (!feat) {
                problem(tr("domain %1 has no Wave Equation feature").arg(d + 1));
                continue;
            }
            const bool user = feat->text(QStringLiteral("source")) == QLatin1String("user");
            const Node* mat = material[d];
            const QString object = topo.objectNames.value(topo.domains[d].owner);
            if (!user && !mat) {
                problem(tr("domain %1 (%2) has no material: its refractive index or permittivity is needed").arg(d + 1).arg(object));
                continue;
            }
            const Node& from = user ? *feat : *mat;
            auto text = [&](const QString& key) { return user ? feat->text(key) : property(*mat, key); };
            m.byIndex = feat->text(QStringLiteral("constitutive")) != QLatin1String("epsilonr");
            const bool anisotropic = user && feat->text(QStringLiteral("anisotropy")) == QLatin1String("diagonal");
            if (m.byIndex) {
                if (anisotropic) {
                    const char* keys[3] = {"nx", "ny", "nz"};
                    for (int k = 0; k < 3; ++k) m.n[k] = compile(feat->text(QString::fromLatin1(keys[k])), *feat, QString::fromLatin1(keys[k]));
                } else {
                    const QString n = text(QStringLiteral("n"));
                    if (n.trimmed().isEmpty()) {
                        problem(tr("domain %1 (%2): its material, %3, has no refractive index n").arg(d + 1).arg(object, mat ? mat->name() : QString()));
                        continue;
                    }
                    m.n[0] = compile(n, from, QStringLiteral("n"));
                }
                const QString kappa = text(QStringLiteral("kappa"));
                if (!kappa.trimmed().isEmpty()) m.kappa = compile(kappa, from, QStringLiteral("kappa"));
            } else {
                if (anisotropic) {
                    const char* keys[3] = {"epsxx", "epsyy", "epszz"};
                    for (int k = 0; k < 3; ++k) m.eps[k] = compile(feat->text(QString::fromLatin1(keys[k])), *feat, QString::fromLatin1(keys[k]));
                } else {
                    const QString er = text(QStringLiteral("epsilonr"));
                    if (er.trimmed().isEmpty()) {
                        problem(tr("domain %1 (%2): its material, %3, has no relative permittivity").arg(d + 1).arg(object, mat ? mat->name() : QString()));
                        continue;
                    }
                    m.eps[0] = compile(er, from, QStringLiteral("epsilonr"));
                }
                for (const QString& key : {QStringLiteral("tand"), QStringLiteral("sigma")}) {
                    const QString t = text(key);
                    if (!t.trimmed().isEmpty()) (key == QLatin1String("tand") ? m.tand : m.sigma) = compile(t, from, key);
                }
            }
            const QString mur = text(QStringLiteral("mur"));
            if (!mur.trimmed().isEmpty()) m.mur = compile(mur, from, QStringLiteral("mur"));
            if (feat->flag(QStringLiteral("plasma"))) {
                if (!m.byIndex) problem(tr("%1: the plasma dispersion adds to a refractive index (its constitutive relation n)").arg(feat->name()));
                m.carriersN = compile(feat->text(QStringLiteral("Ncarriers")), *feat, QStringLiteral("Ncarriers"));
                m.carriersP = compile(feat->text(QStringLiteral("Pcarriers")), *feat, QStringLiteral("Pcarriers"));
            }
            if (feat->flag(QStringLiteral("thermooptic"))) {
                const QString dndT = user ? feat->text(QStringLiteral("dndT")) : property(*mat, QStringLiteral("dndT"));
                if (dndT.trimmed().isEmpty()) problem(tr("domain %1 (%2): its material has no thermo-optic coefficient dn/dT").arg(d + 1).arg(object));
                else {
                    m.dndT = compile(dndT, from, QStringLiteral("dndT"));
                    m.T = compile(feat->text(QStringLiteral("T")), *feat, QStringLiteral("T"));
                    m.Tref = compile(feat->text(QStringLiteral("Tref")), *feat, QStringLiteral("Tref"));
                }
            }
            m.defined = true;
            if (pml[d]) {
                m.pml = true;
                const QRectF b = topo.domains[d].bounds;
                const double scale = topo.unitScale;
                // It stretches where it reaches past the inner region.
                bool beyond = false;
                if (!inner.isNull()) {
                    const double tol = 1e-9 * std::max(inner.width(), inner.height());
                    beyond = b.left() < inner.left() - tol || b.right() > inner.right() + tol || b.top() < inner.top() - tol ||
                             b.bottom() > inner.bottom() + tol;
                    const double in[4] = {inner.left(), inner.right(), inner.top(), inner.bottom()};
                    const double out[4] = {b.left(), b.right(), b.top(), b.bottom()};
                    for (int k = 0; k < 4; ++k) {
                        m.inner[k] = in[k] * scale;
                        m.outer[k] = out[k] * scale;
                    }
                }
                if (!beyond)
                    problem(tr("domain %1: a perfectly matched layer lies outside the other domains - beside, above or round them").arg(d + 1));
            }
        }
    }
    /// The largest refractive index (real) on these domains, at their middles.
    double largestIndex(Solution& s, const std::vector<char>& on, double omega) const
    {
        double best = 1;
        PointEvaluator e(s);
        const Mesh& mesh = s.mesh();
        for (int t = 0; t < int(mesh.triangles.size()); ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (d < 0 || !on[std::size_t(d)] || domains[std::size_t(d)].pml) continue;
            e.moveTo(t, 1.0 / 3, 1.0 / 3);
            const At a = at(e, d, omega, 1);
            for (int k = 0; k < 3; ++k) best = std::max(best, std::sqrt(std::abs((a.eps[k] * a.mu[k]).real())));
        }
        return best;
    }
};

// ------------------------------------------------------------------ Modes

class ModeProblem : public Problem
{
public:
    ModeProblem(Solution& solution, int field) : Problem(solution, field) {}

    bool setup(QString* error) override;
    void initialize() override
    {
        Field& f = field();
        f.values.assign(std::size_t(2 * f.edges->size() + 2 * f.space->size()), 0.0);
        f.extra.assign(3, 0.0);
    }
    bool solve(const SolveOptions&, QString* error) override
    {
        *error = tr("%1: a mode analysis is solved by a Mode Analysis step").arg(a_node->name());
        return false;
    }
    bool modal() const override { return true; }
    int solveModes(const SolveOptions& options, int count, double guess, QString* error) override;
    void selectMode(int k) override;
    void setGlobals() override;

private:
    Media a_media;
    std::vector<int> a_edgeEq, a_nodeEq;   // equations, -1 none (fixed or not here)
    int a_nEdge = 0, a_n = 0;
    struct Mode {
        cd beta;
        Eigen::VectorXcd x;   // et then ez, by equation
    };
    std::vector<Mode> a_modes;
    double a_k0 = 0;
    int a_selected = -1;
};

bool ModeProblem::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field& f = field();
    const Topology& topo = a_s.topology();
    QString why;
    const QVector<int> domains = resolveSelection(topo, a_s.model(), Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(topo.domains.size(), 0);
    for (int d : domains) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    a_media.setup(*this, a_s, *a_node, f.domains, materials(),
                  [this](const QString& t, const Node& n, const QString& k) { return compile(t, n, k); },
                  [](const Node& m, const QString& k) { return materialProperty(m, k); }, [this](const QString& w) { problem(w); });
    // For the variables: the index (its real part, as asked) by domain.
    Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    coef.c.assign(topo.domains.size(), Expression());
    std::vector<Expression> mur(topo.domains.size());
    for (std::size_t d = 0; d < a_media.domains.size(); ++d) {
        if (a_media.domains[d].defined && a_media.domains[d].byIndex) coef.c[d] = a_media.domains[d].n[0];
        mur[d] = a_media.domains[d].mur;
    }
    coef.named[QStringLiteral("mur")] = mur;
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    // The equations: edges and nodes of these domains, those on a perfect
    // electric conductor (the outside, by default) fixed at nought.
    const EdgeSpace& es = *f.edges;
    const Space& ns = *f.space;
    const Mesh& mesh = a_s.mesh();
    a_edgeEq.assign(std::size_t(es.size()), -1);
    a_nodeEq.assign(std::size_t(ns.size()), -1);
    for (int t = 0; t < ns.elements(); ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        for (int i = 0; i < es.perElement(); ++i) a_edgeEq[std::size_t(es.dofs(t)[i])] = -2;
        for (int i = 0; i < ns.perElement(); ++i) a_nodeEq[std::size_t(ns.dofs(t)[i])] = -2;
    }
    std::vector<int> condition(topo.boundaries.size(), 0);   // 1 PEC, 2 PMC: the last feature's
    auto inside = [&](int d) { return d >= 0 && f.domains[std::size_t(d)]; };
    for (const Node& feature : a_node->children) {
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k || k->selection != Level::Boundary) continue;
        const QVector<int> picked = resolveSelection(topo, a_s.model(), Level::Boundary, feature.selection(), &why);
        const int c = feature.type.endsWith(QLatin1String("/pec")) ? 1 : feature.type.endsWith(QLatin1String("/pmc")) ? 2 : 0;
        if (!c) continue;
        for (int b : picked) {
            const Topology::Boundary& bd = topo.boundaries[std::size_t(b)];
            // The default conductor on the outside only; one chosen anywhere.
            if (feature.isDefault && inside(bd.up) == inside(bd.down)) continue;
            condition[std::size_t(b)] = c;
        }
    }
    // On a conductor: Et's edges and Ez's nodes (an edge's middle too).
    for (int e = 0; e < int(mesh.edges.size()); ++e) {
        const Mesh::Edge& me = mesh.edges[std::size_t(e)];
        if (condition[std::size_t(me.boundary)] != 1) continue;
        for (int dof : es.edgeDofs(me.a, me.b))
            if (dof >= 0 && a_edgeEq[std::size_t(dof)] != -1) a_edgeEq[std::size_t(dof)] = -3;
        for (int dof : ns.edgeDofs(e))
            if (dof >= 0 && a_nodeEq[std::size_t(dof)] != -1) a_nodeEq[std::size_t(dof)] = -3;
    }
    a_n = 0;
    for (int& q : a_edgeEq)
        if (q == -2) q = a_n++;
    a_nEdge = a_n;
    for (int& q : a_nodeEq)
        if (q == -2) q = a_n++;
    if (a_n == 0) {
        *error = tr("%1: nothing to solve - every field is fixed").arg(a_node->name());
        return false;
    }
    return true;
}

int ModeProblem::solveModes(const SolveOptions& o, int count, double guess, QString* error)
{
    Field& f = field();
    const EdgeSpace& es = *f.edges;
    const Space& ns = *f.space;
    const Mesh& mesh = a_s.mesh();
    const double omega = 2 * M_PI * o.frequency;
    a_k0 = omega / C0;
    const double k0 = a_k0;
    if (!(k0 > 0)) {
        *error = tr("%1: a mode analysis wants a frequency more than 0").arg(a_node->name());
        return -1;
    }
    if (!(guess > 0)) guess = a_media.largestIndex(a_s, f.domains, omega);
    const cd sigma = -(k0 * guess) * (k0 * guess);
    // A and B, element by element.
    std::vector<Eigen::Triplet<cd>> ta, tb;
    const int pe = es.perElement(), pn = ns.perElement();
    const int degree = ns.order() == 1 ? 2 : 4;
    const TriangleRule& rule = triangleRule(degree);
    PointEvaluator ev(a_s);
    std::vector<cd> Ae(std::size_t(pe * pe)), Be(std::size_t((pe + pn) * (pe + pn)));
    for (int t = 0; t < ns.elements(); ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        const AffineMap am = affineMap(ns, t);
        std::fill(Ae.begin(), Ae.end(), 0);
        std::fill(Be.begin(), Be.end(), 0);
        EdgeSpace::Basis eb;
        Shape sh;
        for (const auto& q : rule.points) {
            ev.moveTo(t, q[0], q[1]);
            const Media::At m = a_media.at(ev, d, omega, k0);
            const double w = q[2] * std::abs(am.det);
            es.basis(t, am, q[0], q[1], eb);
            shapeAt(ns.order(), q[0], q[1], sh);
            double lx[6], ly[6];
            const double i00 = am.grad[1][0], i01 = am.grad[2][0], i10 = am.grad[1][1], i11 = am.grad[2][1];
            for (int j = 0; j < pn; ++j) {
                // ∇L from the reference gradient: ∇ = (dxi, deta) through ∇λ1, ∇λ2.
                lx[j] = sh.dxi[j] * i00 + sh.deta[j] * i01;
                ly[j] = sh.dxi[j] * i10 + sh.deta[j] * i11;
            }
            const cd ix = 1.0 / m.mu[1], iy = 1.0 / m.mu[0], iz = 1.0 / m.mu[2];   // (x parts with 1/µyy, y parts 1/µxx)
            for (int i = 0; i < pe; ++i)
                for (int j = 0; j < pe; ++j) {
                    Ae[std::size_t(i * pe + j)] += w * (eb.curl[i] * eb.curl[j] * iz - k0 * k0 * (m.eps[0] * eb.vx[i] * eb.vx[j] + m.eps[1] * eb.vy[i] * eb.vy[j]));
                    Be[std::size_t(i * (pe + pn) + j)] += w * (eb.vx[i] * eb.vx[j] * ix + eb.vy[i] * eb.vy[j] * iy);
                }
            for (int i = 0; i < pe; ++i)
                for (int j = 0; j < pn; ++j) {
                    const cd g = w * (eb.vx[i] * lx[j] * ix + eb.vy[i] * ly[j] * iy);
                    Be[std::size_t(i * (pe + pn) + pe + j)] += g;
                    Be[std::size_t((pe + j) * (pe + pn) + i)] += g;
                }
            for (int i = 0; i < pn; ++i)
                for (int j = 0; j < pn; ++j)
                    Be[std::size_t((pe + i) * (pe + pn) + pe + j)] +=
                        w * (lx[i] * lx[j] * ix + ly[i] * ly[j] * iy - k0 * k0 * m.eps[2] * sh.n[i] * sh.n[j]);
        }
        int eq[14];
        for (int i = 0; i < pe; ++i) eq[i] = a_edgeEq[std::size_t(es.dofs(t)[i])];
        for (int i = 0; i < pn; ++i) eq[pe + i] = a_nodeEq[std::size_t(ns.dofs(t)[i])];
        for (int i = 0; i < pe; ++i)
            for (int j = 0; j < pe; ++j)
                if (eq[i] >= 0 && eq[j] >= 0) ta.emplace_back(eq[i], eq[j], Ae[std::size_t(i * pe + j)]);
        for (int i = 0; i < pe + pn; ++i)
            for (int j = 0; j < pe + pn; ++j)
                if (eq[i] >= 0 && eq[j] >= 0 && Be[std::size_t(i * (pe + pn) + j)] != cd(0)) tb.emplace_back(eq[i], eq[j], Be[std::size_t(i * (pe + pn) + j)]);
    }
    Eigen::SparseMatrix<cd> A(a_n, a_n), B(a_n, a_n);
    A.setFromTriplets(ta.begin(), ta.end());
    B.setFromTriplets(tb.begin(), tb.end());
    Eigen::SparseMatrix<cd> M = A - sigma * B;
    M.makeCompressed();
    Eigen::SparseLU<Eigen::SparseMatrix<cd>> lu;
    lu.analyzePattern(M);
    lu.factorize(M);
    if (lu.info() != Eigen::Success) {
        *error = tr("%1: the shifted matrix is singular - %2 (another effective index to search about)")
                     .arg(a_node->name(), QString::fromStdString(lu.lastErrorMessage()));
        return -1;
    }
    // With absorbing layers, more than asked: the layers' own modes (their
    // fields in the layers mostly) are left out.
    bool layers = false;
    for (std::size_t d = 0; d < a_media.domains.size(); ++d) layers = layers || (f.domains[d] && a_media.domains[d].pml);
    const int wanted = std::max(1, count);
    // The share of a mode's |E|² in the layers.
    auto inLayers = [&](const Eigen::VectorXcd& x, cd beta) {
        double all = 0, there = 0;
        EdgeSpace::Basis eb;
        Shape sh;
        const TriangleRule& q2 = triangleRule(2);
        for (int t = 0; t < ns.elements(); ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (d < 0 || !f.domains[std::size_t(d)]) continue;
            const AffineMap am = affineMap(ns, t);
            double e2 = 0;
            for (const auto& q : q2.points) {
                es.basis(t, am, q[0], q[1], eb);
                shapeAt(ns.order(), q[0], q[1], sh);
                cd ex = 0, ey = 0, ez = 0;
                for (int i = 0; i < pe; ++i) {
                    const int k = a_edgeEq[std::size_t(es.dofs(t)[i])];
                    if (k < 0) continue;
                    ex += x(k) * eb.vx[i];
                    ey += x(k) * eb.vy[i];
                }
                for (int i = 0; i < pn; ++i) {
                    const int k = a_nodeEq[std::size_t(ns.dofs(t)[i])];
                    if (k >= 0) ez += x(k) * sh.n[i];
                }
                e2 += q[2] * std::abs(am.det) * ((std::norm(ex) + std::norm(ey)) / std::norm(beta) + std::norm(ez));
            }
            all += e2;
            if (a_media.domains[std::size_t(d)].pml) there += e2;
        }
        return all > 0 ? there / all : 0.0;
    };
    // (As many again while too few are left, the layers' crowding the guess.)
    int dropped = 0, products = 0, restarts = 0;
    for (int asked = layers ? 2 * wanted + 4 : wanted;; asked *= 2) {
        asked = std::min(asked, std::max(1, a_n - 2));
        const EigenResult r = largestEigenvalues(a_n, asked, [&](const Eigen::VectorXcd& x, Eigen::VectorXcd& y) { y = lu.solve(B * x); });
        if (!r.error.isEmpty()) a_s.log << tr("Warning: %1: %2").arg(a_node->name(), r.error);
        products += r.products;
        restarts += r.restarts;
        a_modes.clear();
        dropped = 0;
        for (std::size_t i = 0; i < r.values.size(); ++i) {
            const cd lambda = sigma + 1.0 / r.values[i];
            // (The null space of A - its Ez alone, λ nought - is no mode.)
            if (std::abs(lambda) < 1e-8 * k0 * k0) continue;
            cd beta = std::sqrt(-lambda);
            if (beta.real() < 0) beta = -beta;
            if (layers && inLayers(r.vectors[i], beta) > 0.5) {
                ++dropped;
                continue;
            }
            // Its phase, which the eigensolver leaves to chance: its largest
            // transverse value real and more than nought.
            Eigen::VectorXcd x = r.vectors[i];
            Eigen::Index largest = 0;
            if (a_nEdge > 0) x.head(a_nEdge).cwiseAbs().maxCoeff(&largest);
            if (std::abs(x(largest)) > 0) x *= std::conj(x(largest)) / std::abs(x(largest));
            a_modes.push_back({beta, x});
        }
        if (!layers || int(a_modes.size()) >= wanted || asked >= 96 || asked >= a_n - 2) break;
    }
    // By their effective index, the highest first; those nearest the guess
    // as many as asked.
    if (int(a_modes.size()) > wanted) {
        std::sort(a_modes.begin(), a_modes.end(), [&](const Mode& a, const Mode& b) {
            return std::abs(a.beta * a.beta + sigma) < std::abs(b.beta * b.beta + sigma);
        });
        a_modes.resize(std::size_t(wanted));
    }
    std::sort(a_modes.begin(), a_modes.end(), [](const Mode& a, const Mode& b) { return a.beta.real() > b.beta.real(); });
    if (dropped) a_s.log << tr("%1: %2 modes of the absorbing layers left out").arg(a_node->name()).arg(dropped);
    a_s.log << tr("%1: %2 modes at %3 about n = %4 (%5 products, %6 restarts)")
                   .arg(a_node->name())
                   .arg(a_modes.size())
                   .arg(formatQuantity(o.frequency, Dim::of(0, 0, -1)), formatNumber(guess, 5))
                   .arg(products)
                   .arg(restarts);
    f.solved = true;
    return int(a_modes.size());
}

void ModeProblem::selectMode(int k)
{
    Field& f = field();
    if (k < 0 || k >= int(a_modes.size())) return;
    a_selected = k;
    const Mode& md = a_modes[std::size_t(k)];
    const int ne = f.edges->size(), nn = f.space->size();
    f.values.assign(std::size_t(2 * ne + 2 * nn), 0.0);
    // Et = et / β, Ez = j ez.
    for (int dof = 0; dof < ne; ++dof)
        if (a_edgeEq[std::size_t(dof)] >= 0) {
            const cd v = md.x(a_edgeEq[std::size_t(dof)]) / md.beta;
            f.values[std::size_t(dof)] = v.real();
            f.values[std::size_t(ne + dof)] = v.imag();
        }
    for (int dof = 0; dof < nn; ++dof)
        if (a_nodeEq[std::size_t(dof)] >= 0) {
            const cd v = J * md.x(a_nodeEq[std::size_t(dof)]);
            f.values[std::size_t(2 * ne + dof)] = v.real();
            f.values[std::size_t(2 * ne + nn + dof)] = v.imag();
        }
    f.extra = {md.beta.real(), md.beta.imag(), a_k0};
    // Its power along z, 1 W: Sz = ½ Re(Ex Hy* - Ey Hx*) over the domains.
    double power = 0, largest = 0;
    const Mesh& mesh = a_s.mesh();
    PointEvaluator ev(a_s);
    const TriangleRule& rule = triangleRule(f.space->order() == 1 ? 2 : 4);
    for (int t = 0; t < f.space->elements(); ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)] || a_media.domains[std::size_t(d)].pml) continue;
        const AffineMap am = affineMap(*f.space, t);
        for (const auto& q : rule.points) {
            ev.moveTo(t, q[0], q[1]);
            const ModeFields mf = modeFieldsAt(ev, a_fi);
            if (!mf.defined) continue;
            power += q[2] * std::abs(am.det) * 0.5 * (mf.E[0] * std::conj(mf.H[1]) - mf.E[1] * std::conj(mf.H[0])).real();
            largest = std::max(largest, std::sqrt(std::norm(mf.E[0]) + std::norm(mf.E[1]) + std::norm(mf.E[2])));
        }
    }
    const double scale = power > 1e-300 ? 1 / std::sqrt(power) : largest > 0 ? 1 / largest : 1.0;
    for (double& v : f.values) v *= scale;
    a_s.setMode(k + 1);
}

void ModeProblem::setGlobals()
{
    if (a_selected < 0 || a_selected >= int(a_modes.size())) return;
    const cd beta = a_modes[std::size_t(a_selected)].beta;
    const QString p = field().tag + QLatin1Char('.');
    const Dim perMetre = Dim::of(-1);
    a_s.setGlobal(p + QStringLiteral("neff"), beta.real() / a_k0, Dim(), tr("effective index: β/k0"));
    a_s.setGlobal(p + QStringLiteral("beta"), beta.real(), perMetre, tr("propagation constant β"));
    a_s.setGlobal(p + QStringLiteral("alpha"), -beta.imag(), perMetre, tr("attenuation constant α (Np/m)"));
    a_s.setGlobal(p + QStringLiteral("loss"), -beta.imag() * 20 / std::log(10.0), perMetre, tr("loss in dB per metre"));
    a_s.setGlobal(p + QStringLiteral("neff_im"), -beta.imag() / a_k0, Dim(), tr("effective index's imaginary part, its loss (β = k0 (neff - j neff_im))"));
    a_s.setGlobal(p + QStringLiteral("lambda0"), 2 * M_PI / a_k0, Dim::length(), tr("free-space wavelength"));
    a_s.setGlobal(p + QStringLiteral("mode"), a_selected + 1, Dim(), tr("the mode's number, by effective index"));
}

// ------------------------------------------------------------------ Waves in the plane

class WaveProblem : public Problem
{
public:
    WaveProblem(Solution& solution, int field) : Problem(solution, field) {}

    bool setup(QString* error) override;
    void initialize() override
    {
        Field& f = field();
        f.values.assign(std::size_t(2 * f.space->size()), 0.0);
    }
    bool solve(const SolveOptions& options, QString* error) override;
    void setGlobals() override;
    bool harmonic() const override { return true; }

private:
    struct Port {
        QString name;
        bool excite = true;
        std::vector<int> edges;     // mesh edges
        cd beta;
        Eigen::VectorXcd c;         // ∫ p e N_i ds, by equation
    };
    /// The point of triangle \a t on mesh edge \a e at s (0 at its a).
    void edgePoint(int e, double s, int* t, double* xi, double* eta) const;
    /// p (scalar: the adjacent medium's) and k at a point.
    void scalars(PointEvaluator& ev, int d, double omega, double k0, cd* p, cd* q) const;
    bool portMode(Port& port, double omega, double k0, QString* error);

    Media a_media;
    bool a_hz = false;
    std::vector<int> a_eq;          // by dof: equation, -1 none, -3 fixed (0)
    int a_neq = 0;
    std::vector<int> a_condition;   // by boundary: 1 fixed, 2 natural, 3 scattering
    struct Incident {
        bool plane = false;
        Expression amplitude;
        double angle = 0;
    };
    std::vector<Incident> a_incident;   // by boundary
    std::vector<Port> a_ports;
    // The S-matrix found (by excited port's column).
    std::vector<std::vector<cd>> a_S;
    std::vector<int> a_fed;
    double a_omega = 0;
};

bool WaveProblem::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field& f = field();
    const Topology& topo = a_s.topology();
    const Mesh& mesh = a_s.mesh();
    a_hz = a_node->text(QStringLiteral("polarization")) == QLatin1String("hz");
    QString why;
    const QVector<int> domains = resolveSelection(topo, a_s.model(), Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(topo.domains.size(), 0);
    for (int d : domains) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    a_media.setup(*this, a_s, *a_node, f.domains, materials(),
                  [this](const QString& t, const Node& n, const QString& k) { return compile(t, n, k); },
                  [](const Node& m, const QString& k) { return materialProperty(m, k); }, [this](const QString& w) { problem(w); });
    Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    coef.c.assign(topo.domains.size(), Expression());
    std::vector<Expression> mur(topo.domains.size()), epsr(topo.domains.size());
    for (std::size_t d = 0; d < a_media.domains.size(); ++d) {
        const Media::Domain& m = a_media.domains[d];
        if (!m.defined) continue;
        if (m.byIndex) {
            coef.c[d] = m.n[0];
            if (m.n[0].isValid()) epsr[d] = Expression::compile(QStringLiteral("(%1)^2").arg(m.n[0].text()), a_s.pointScope());
        } else {
            epsr[d] = m.eps[0];
        }
        mur[d] = m.mur;
    }
    coef.named[QStringLiteral("mur")] = mur;
    coef.named[QStringLiteral("epsr")] = epsr;
    // Boundaries: conductors, scattering, ports (the last feature's).
    a_condition.assign(topo.boundaries.size(), 0);
    a_incident.assign(topo.boundaries.size(), Incident());
    a_ports.clear();
    std::vector<int> portOf(topo.boundaries.size(), -1);
    auto inside = [&](int d) { return d >= 0 && f.domains[std::size_t(d)]; };
    for (const Node& feature : a_node->children) {
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k || k->selection != Level::Boundary) continue;
        const QVector<int> picked = resolveSelection(topo, a_s.model(), Level::Boundary, feature.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(feature.name(), why));
        const QString t = feature.type;
        if (t.endsWith(QLatin1String("/pec")) || t.endsWith(QLatin1String("/pmc"))) {
            // Ez: a conductor fixes it, Hz: a magnetic conductor does.
            const bool pec = t.endsWith(QLatin1String("/pec"));
            for (int b : picked) {
                const Topology::Boundary& bd = topo.boundaries[std::size_t(b)];
                if (feature.isDefault && inside(bd.up) == inside(bd.down)) continue;
                a_condition[std::size_t(b)] = pec != a_hz ? 1 : 2;
                portOf[std::size_t(b)] = -1;
            }
        } else if (t.endsWith(QLatin1String("/scattering"))) {
            Incident in;
            if (feature.text(QStringLiteral("incident")) == QLatin1String("plane")) {
                in.plane = true;
                in.amplitude = compile(feature.text(QStringLiteral("E0")), feature, QStringLiteral("E0"), false);
                const std::optional<double> a = evaluateConstant(feature.text(QStringLiteral("angle")), a_s.parameters().scope, Dim(), &why);
                in.angle = a ? *a * M_PI / 180 : 0;
            }
            for (int b : picked) {
                a_condition[std::size_t(b)] = 3;
                a_incident[std::size_t(b)] = in;
                portOf[std::size_t(b)] = -1;
            }
        } else if (t.endsWith(QLatin1String("/port"))) {
            Port port;
            port.name = feature.text(QStringLiteral("name")).trimmed();
            if (port.name.isEmpty()) port.name = QString::number(a_ports.size() + 1);
            for (const Port& other : a_ports)
                if (other.name == port.name) problem(tr("%1: another port is named %2").arg(feature.name(), port.name));
            port.excite = feature.flag(QStringLiteral("excite"));
            for (int b : picked) {
                a_condition[std::size_t(b)] = 4;
                portOf[std::size_t(b)] = int(a_ports.size());
            }
            a_ports.push_back(port);
        }
    }
    for (int e = 0; e < int(mesh.edges.size()); ++e) {
        const int b = mesh.edges[std::size_t(e)].boundary;
        if (portOf[std::size_t(b)] >= 0) a_ports[std::size_t(portOf[std::size_t(b)])].edges.push_back(e);
    }
    for (const Port& p : a_ports)
        if (p.edges.empty()) problem(tr("port %1 is on no boundary").arg(p.name));
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    // The equations: u fixed at nought where a conductor fixes it.
    const Space& sp = *f.space;
    a_eq.assign(std::size_t(sp.size()), -1);
    for (int t = 0; t < sp.elements(); ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        for (int i = 0; i < sp.perElement(); ++i) a_eq[std::size_t(sp.dofs(t)[i])] = -2;
    }
    for (int e = 0; e < int(mesh.edges.size()); ++e)
        if (a_condition[std::size_t(mesh.edges[std::size_t(e)].boundary)] == 1)
            for (int dof : sp.edgeDofs(e))
                if (dof >= 0 && a_eq[std::size_t(dof)] != -1) a_eq[std::size_t(dof)] = -3;
    a_neq = 0;
    for (int& q : a_eq)
        if (q == -2) q = a_neq++;
    return true;
}

void WaveProblem::edgePoint(int e, double s, int* t, double* xi, double* eta) const
{
    const Mesh& mesh = a_s.mesh();
    const Field& fld = a_s.fields()[std::size_t(a_fi)];
    const Mesh::Edge& me = mesh.edges[std::size_t(e)];
    *t = -1;
    for (int side : {me.left, me.right})
        if (side >= 0 && fld.domains[std::size_t(mesh.domain[std::size_t(side)])]) {
            *t = side;
            break;
        }
    if (*t < 0) return;
    const std::array<int, 3>& tv = mesh.triangles[std::size_t(*t)];
    const int k = Space::localEdge(tv, me.a, me.b);
    const double r = tv[std::size_t(k)] == me.a ? s : 1 - s;
    *xi = k == 0 ? r : k == 1 ? 1 - r : 0;
    *eta = k == 0 ? 0 : k == 1 ? r : 1 - r;
}

void WaveProblem::scalars(PointEvaluator& ev, int d, double omega, double k0, cd* p, cd* q) const
{
    const Media::At m = a_media.at(ev, d, omega, k0);
    // (On a boundary: the medium's own, its isotropic part.)
    *p = a_hz ? 1.0 / m.eps[2] : 1.0 / m.mu[2];
    *q = a_hz ? m.mu[2] : m.eps[2];
}

bool WaveProblem::portMode(Port& port, double omega, double k0, QString* error)
{
    // Its line's own problem: -(p e')' - k0² q e = -β² p e, on the 2D
    // degrees of freedom along it (those fixed at its ends left out).
    const Field& fld = a_s.fields()[std::size_t(a_fi)];
    const Space& sp = *fld.space;
    const Mesh& mesh = a_s.mesh();
    std::map<int, int> local;
    for (int e : port.edges)
        for (int dof : sp.edgeDofs(e))
            if (dof >= 0 && a_eq[std::size_t(dof)] >= 0 && !local.count(dof)) local.emplace(dof, int(local.size()));
    const int n = int(local.size());
    if (n < 1) {
        *error = tr("port %1: nothing free on it").arg(port.name);
        return false;
    }
    Eigen::MatrixXcd K = Eigen::MatrixXcd::Zero(n, n), M = Eigen::MatrixXcd::Zero(n, n);
    PointEvaluator ev(a_s);
    const int ne = sp.order() == 1 ? 2 : 3;
    for (int e : port.edges) {
        const std::array<int, 3> dofs = sp.edgeDofs(e);
        for (const auto& q : lineRule(sp.order() + 2)) {
            double jac = 0;
            sp.edgePoint(e, q[0], &jac);
            int t = -1;
            double xi = 0, eta = 0;
            edgePoint(e, q[0], &t, &xi, &eta);
            if (t < 0) continue;
            ev.moveTo(t, xi, eta);
            cd p, qq;
            scalars(ev, mesh.domain[std::size_t(t)], omega, k0, &p, &qq);
            const double s = q[0];
            double N[3], dN[3];
            if (ne == 2) {
                N[0] = 1 - s;
                N[1] = s;
                dN[0] = -1;
                dN[1] = 1;
            } else {
                N[0] = (1 - s) * (1 - 2 * s);
                N[1] = s * (2 * s - 1);
                N[2] = 4 * s * (1 - s);
                dN[0] = 4 * s - 3;
                dN[1] = 4 * s - 1;
                dN[2] = 4 - 8 * s;
            }
            const double w = q[1] * jac;
            for (int i = 0; i < ne; ++i) {
                auto ii = local.find(dofs[std::size_t(i)]);
                if (ii == local.end()) continue;
                for (int j = 0; j < ne; ++j) {
                    auto jj = local.find(dofs[std::size_t(j)]);
                    if (jj == local.end()) continue;
                    // (d/ds: d/dq over the length per unit q)
                    K(ii->second, jj->second) += w * (p * dN[i] * dN[j] / (jac * jac) - k0 * k0 * qq * N[i] * N[j]);
                    M(ii->second, jj->second) += w * p * N[i] * N[j];
                }
            }
        }
    }
    Eigen::ComplexEigenSolver<Eigen::MatrixXcd> es(M.fullPivLu().solve(K));
    int best = -1;
    for (int i = 0; i < n; ++i)
        if (best < 0 || (-es.eigenvalues()(i)).real() > (-es.eigenvalues()(best)).real()) best = i;
    const cd beta2 = -es.eigenvalues()(best);
    if (!(beta2.real() > 0)) {
        *error = tr("port %1: no mode propagates at %2 (its first is cut off)").arg(port.name, formatQuantity(omega / (2 * M_PI), Dim::of(0, 0, -1)));
        return false;
    }
    port.beta = std::sqrt(beta2);
    if (port.beta.real() < 0) port.beta = -port.beta;
    Eigen::VectorXcd e = es.eigenvectors().col(best);
    // ∫ p e e ds = 1 (no conjugate: the bilinear form of the port's condition).
    const cd norm = std::sqrt((e.transpose() * M * e)(0));
    e /= norm;
    // Its sign: ∫ p e ds more than nought (the first mode does not change
    // sign along the port), so that S21's phase does not turn with the mesh.
    const cd along = (M * e).sum();
    if ((std::abs(along.real()) >= std::abs(along.imag()) ? along.real() : along.imag()) < 0) e = -e;
    // c: ∫ p e N_i ds by the 2D equation.
    port.c = Eigen::VectorXcd::Zero(a_neq);
    const Eigen::VectorXcd Me = M * e;
    for (const auto& [dof, i] : local) port.c(a_eq[std::size_t(dof)]) = Me(i);
    return true;
}

bool WaveProblem::solve(const SolveOptions& o, QString* error)
{
    Field& fld = field();
    if (!(o.harmonic && o.frequency > 0)) {
        *error = tr("%1: waves are solved by a Frequency Domain step, at a frequency more than 0").arg(a_node->name());
        return false;
    }
    const Space& sp = *fld.space;
    const Mesh& mesh = a_s.mesh();
    const double omega = 2 * M_PI * o.frequency;
    a_omega = omega;
    const double k0 = omega / C0;
    const int per = sp.perElement();
    std::vector<Eigen::Triplet<cd>> trip;
    Eigen::VectorXcd F = Eigen::VectorXcd::Zero(a_neq);
    PointEvaluator ev(a_s);
    const TriangleRule& rule = triangleRule(sp.order() == 1 ? 2 : 4);
    Shape sh;
    double gx[6], gy[6];
    std::vector<cd> Ke(std::size_t(per * per));
    for (int t = 0; t < sp.elements(); ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !fld.domains[std::size_t(d)]) continue;
        std::fill(Ke.begin(), Ke.end(), 0);
        const int* dofs = sp.dofs(t);
        for (const auto& q : rule.points) {
            shapeAt(sp.order(), q[0], q[1], sh);
            const Space::Map m = sp.map(t, q[0], q[1]);
            ev.moveTo(t, q[0], q[1]);
            const Media::At md = a_media.at(ev, d, omega, k0);
            // Ez: P = diag(1/µyy, 1/µxx), q = εzz; Hz: from ε, q = µzz.
            const cd px = a_hz ? 1.0 / md.eps[1] : 1.0 / md.mu[1];
            const cd py = a_hz ? 1.0 / md.eps[0] : 1.0 / md.mu[0];
            const cd qq = a_hz ? md.mu[2] : md.eps[2];
            const double w = q[2] * std::abs(m.det);
            for (int i = 0; i < per; ++i) Space::gradient(m, sh.dxi[i], sh.deta[i], gx[i], gy[i]);
            for (int i = 0; i < per; ++i)
                for (int j = 0; j < per; ++j) Ke[std::size_t(i * per + j)] += w * (px * gx[i] * gx[j] + py * gy[i] * gy[j] - k0 * k0 * qq * sh.n[i] * sh.n[j]);
        }
        for (int i = 0; i < per; ++i) {
            const int qi = a_eq[std::size_t(dofs[i])];
            if (qi < 0) continue;
            for (int j = 0; j < per; ++j) {
                const int qj = a_eq[std::size_t(dofs[j])];
                if (qj >= 0) trip.emplace_back(qi, qj, Ke[std::size_t(i * per + j)]);
            }
        }
    }
    // Scattering boundaries: jk p u v, and what a plane wave brings in.
    const int ne = sp.order() == 1 ? 2 : 3;
    for (int e = 0; e < int(mesh.edges.size()); ++e) {
        const Mesh::Edge& me = mesh.edges[std::size_t(e)];
        if (a_condition[std::size_t(me.boundary)] != 3) continue;
        const Incident& in = a_incident[std::size_t(me.boundary)];
        const std::array<int, 3> dofs = sp.edgeDofs(e);
        int tri = -1;
        double xi0 = 0, eta0 = 0;
        edgePoint(e, 0.5, &tri, &xi0, &eta0);
        if (tri < 0) continue;
        // Its outward normal: away from the triangle it is of.
        const QPointF a = sp.point(dofs[0]), b = sp.point(dofs[1]);
        QPointF nrm(b.y() - a.y(), -(b.x() - a.x()));
        const double len = std::hypot(nrm.x(), nrm.y());
        nrm /= len;
        const auto& tv = mesh.triangles[std::size_t(tri)];
        QPointF centre;
        for (int k = 0; k < 3; ++k) centre += mesh.nodes[std::size_t(tv[std::size_t(k)])] * mesh.unitScale / 3.0;
        if ((centre.x() - a.x()) * nrm.x() + (centre.y() - a.y()) * nrm.y() > 0) nrm = -nrm;
        for (const auto& q : lineRule(sp.order() + 2)) {
            double jac = 0;
            const QPointF x = sp.edgePoint(e, q[0], &jac);
            int t = -1;
            double xi = 0, eta = 0;
            edgePoint(e, q[0], &t, &xi, &eta);
            ev.moveTo(t, xi, eta);
            cd p, qq;
            scalars(ev, mesh.domain[std::size_t(t)], omega, k0, &p, &qq);
            const cd k = k0 * std::sqrt(qq / p);
            const double s = q[0];
            double N[3];
            if (ne == 2) {
                N[0] = 1 - s;
                N[1] = s;
            } else {
                N[0] = (1 - s) * (1 - 2 * s);
                N[1] = s * (2 * s - 1);
                N[2] = 4 * s * (1 - s);
            }
            const double w = q[1] * jac;
            cd g = 0;
            if (in.plane) {
                const double amp = in.amplitude.isConstant() ? in.amplitude.constant() : ev.eval(in.amplitude);
                const double dx = std::cos(in.angle), dy = std::sin(in.angle);
                const cd uinc = amp * std::exp(-J * k * (x.x() * dx + x.y() * dy));
                const cd dn = -J * k * (nrm.x() * dx + nrm.y() * dy) * uinc;
                g = p * (dn + J * k * uinc);
            }
            for (int i = 0; i < ne; ++i) {
                const int qi = a_eq[std::size_t(dofs[std::size_t(i)])];
                if (qi < 0) continue;
                F(qi) += w * g * N[i];
                for (int j = 0; j < ne; ++j) {
                    const int qj = a_eq[std::size_t(dofs[std::size_t(j)])];
                    if (qj >= 0) trip.emplace_back(qi, qj, w * J * k * p * N[i] * N[j]);
                }
            }
        }
    }
    // Ports: their modes, jβ c cᵀ each.
    for (Port& port : a_ports) {
        if (!portMode(port, omega, k0, error)) return false;
        std::vector<int> nz;
        for (int i = 0; i < a_neq; ++i)
            if (port.c(i) != cd(0)) nz.push_back(i);
        for (int i : nz)
            for (int j : nz) trip.emplace_back(i, j, J * port.beta * port.c(i) * port.c(j));
    }
    Eigen::SparseMatrix<cd> A(a_neq, a_neq);
    A.setFromTriplets(trip.begin(), trip.end());
    A.makeCompressed();
    Eigen::SparseLU<Eigen::SparseMatrix<cd>> lu;
    lu.analyzePattern(A);
    lu.factorize(A);
    if (lu.info() != Eigen::Success) {
        *error = tr("%1: the matrix is singular - %2").arg(a_node->name(), QString::fromStdString(lu.lastErrorMessage()));
        return false;
    }
    // Fed: each port that is, in turn (the field kept: the first's, the plane
    // wave's when none is).
    a_fed.clear();
    for (int i = 0; i < int(a_ports.size()); ++i)
        if (a_ports[std::size_t(i)].excite) a_fed.push_back(i);
    a_S.assign(a_ports.size(), std::vector<cd>(a_ports.size(), cd(NaN, NaN)));
    Eigen::VectorXcd kept;
    for (std::size_t j = 0; j < a_fed.size() || (j == 0 && a_fed.empty()); ++j) {
        Eigen::VectorXcd rhs = F;
        const int fed = a_fed.empty() ? -1 : a_fed[j];
        if (fed >= 0) {
            const Port& pj = a_ports[std::size_t(fed)];
            rhs += 2.0 * J * pj.beta * pj.c;
            if (j > 0) rhs -= F;   // (a plane wave with the first only)
        }
        const Eigen::VectorXcd x = lu.solve(rhs);
        if (!x.allFinite()) {
            *error = tr("%1: the solution is not finite").arg(a_node->name());
            return false;
        }
        if (j == 0) kept = x;
        if (fed < 0) break;
        for (int i = 0; i < int(a_ports.size()); ++i) {
            const Port& pi = a_ports[std::size_t(i)];
            const cd bi = (pi.c.transpose() * x)(0) - (i == fed ? cd(1) : cd(0));
            a_S[std::size_t(i)][std::size_t(fed)] = bi * std::sqrt(pi.beta / a_ports[std::size_t(fed)].beta);
        }
    }
    const int nd = sp.size();
    fld.values.assign(std::size_t(2 * nd), 0.0);
    for (int dof = 0; dof < nd; ++dof) {
        const int q = a_eq[std::size_t(dof)];
        if (q < 0) continue;
        fld.values[std::size_t(dof)] = kept(q).real();
        fld.values[std::size_t(nd + dof)] = kept(q).imag();
    }
    fld.terminals.clear();
    for (const Port& p : a_ports) {
        Field::Terminal t;
        t.name = p.name;
        t.potential = p.beta.real();
        t.potentialIm = p.beta.imag();
        fld.terminals.push_back(t);
    }
    fld.solved = true;
    return true;
}

void WaveProblem::setGlobals()
{
    const QString p = field().tag + QLatin1Char('.');
    for (int i = 0; i < int(a_ports.size()); ++i) {
        const QString a = a_ports[std::size_t(i)].name;
        a_s.setGlobal(p + QStringLiteral("beta_") + a, a_ports[std::size_t(i)].beta.real(), Dim::of(-1), tr("port %1's mode, its propagation constant").arg(a));
        for (int j : a_fed) {
            const QString b = a_ports[std::size_t(j)].name;
            const cd v = a_S[std::size_t(i)][std::size_t(j)];
            const bool shortNames = a.size() == 1 && b.size() == 1;
            const QString base = shortNames ? p + QStringLiteral("S") + a + b : p + QStringLiteral("S_") + a + QLatin1Char('_') + b;
            const QString what = tr("S-parameter %1 %2").arg(a, b);
            a_s.setGlobal(base, v.real(), Dim(), what + tr(", its real part"));
            a_s.setGlobal(base + QStringLiteral("_im"), v.imag(), Dim(), what + tr(", its imaginary part"));
            a_s.setGlobal(base + QStringLiteral("dB"), 20 * std::log10(std::abs(v)), Dim(), what + tr(" in dB"));
            a_s.setGlobal(base + QStringLiteral("_abs"), std::abs(v), Dim(), what + tr(", its magnitude"));
            a_s.setGlobal(base + QStringLiteral("_arg"), std::arg(v) * 180 / M_PI, Dim(), what + tr(", its phase in degrees"));
        }
    }
}

} // namespace

WaveFields waveFieldsAt(PointEvaluator& e, int f)
{
    WaveFields out;
    const Solution& s = e.solution();
    const Field& fld = s.fields()[std::size_t(f)];
    double u = 0, ux = 0, uy = 0, v = 0, vx = 0, vy = 0;
    e.component(f, 0, u, ux, uy);
    e.component(f, 1, v, vx, vy);
    if (!std::isfinite(u)) return out;
    out.u = cd(u, v);
    out.ux = cd(ux, vx);
    out.uy = cd(uy, vy);
    const double omega = 2 * M_PI * s.frequency();
    if (!(omega > 0)) return out;
    double mur = e.coefficient(f, QStringLiteral("mur"));
    if (!std::isfinite(mur) || mur <= 0) mur = 1;
    double epsr = e.coefficient(f, QStringLiteral("epsr"));
    if (!std::isfinite(epsr) || epsr == 0) epsr = 1;
    const bool hz = fld.variable == QLatin1String("Hz");
    if (!hz) {
        // E = ẑ Ez; H = j/(ωµ) ∇×E = j/(ωµ) (∂y Ez, -∂x Ez, 0).
        const cd f0 = J / (omega * Mu0 * mur);
        out.E[2] = out.u;
        out.H[0] = f0 * out.uy;
        out.H[1] = -f0 * out.ux;
    } else {
        // H = ẑ Hz; E = (1/jωε) ∇×H = -j/(ωε) (∂y Hz, -∂x Hz, 0).
        const cd f0 = -J / (omega * 8.8541878188e-12 * epsr);
        out.H[2] = out.u;
        out.E[0] = f0 * out.uy;
        out.E[1] = -f0 * out.ux;
    }
    out.defined = true;
    return out;
}

std::unique_ptr<Problem> makeWaveProblem(Solution& solution, int field)
{
    return std::make_unique<WaveProblem>(solution, field);
}

ModeFields modeFieldsAt(PointEvaluator& e, int f)
{
    ModeFields out;
    const Solution& s = e.solution();
    const Field& fld = s.fields()[std::size_t(f)];
    const int t = e.triangle();
    if (t < 0 || !fld.edges || fld.extra.size() < 3) return out;
    const int d = e.domain();
    if (d < 0 || !fld.domains[std::size_t(d)]) return out;
    const EdgeSpace& es = *fld.edges;
    const Space& ns = *fld.space;
    const int ne = es.size(), nn = ns.size();
    if (int(fld.values.size()) < 2 * ne + 2 * nn) return out;
    const AffineMap am = affineMap(ns, t);
    EdgeSpace::Basis b;
    es.basis(t, am, e.xi(), e.eta(), b);
    cd ex = 0, ey = 0, curl = 0;
    for (int i = 0; i < b.count; ++i) {
        const int dof = es.dofs(t)[i];
        const cd v(fld.values[std::size_t(dof)], fld.values[std::size_t(ne + dof)]);
        ex += b.vx[i] * v;
        ey += b.vy[i] * v;
        curl += b.curl[i] * v;
    }
    Shape sh;
    shapeAt(ns.order(), e.xi(), e.eta(), sh);
    cd ez = 0, ezx = 0, ezy = 0;
    for (int i = 0; i < sh.count; ++i) {
        const int dof = ns.dofs(t)[i];
        const cd v(fld.values[std::size_t(2 * ne + dof)], fld.values[std::size_t(2 * ne + nn + dof)]);
        ez += sh.n[i] * v;
        ezx += (sh.dxi[i] * am.grad[1][0] + sh.deta[i] * am.grad[2][0]) * v;
        ezy += (sh.dxi[i] * am.grad[1][1] + sh.deta[i] * am.grad[2][1]) * v;
    }
    const cd beta(fld.extra[0], fld.extra[1]);
    const double k0 = fld.extra[2];
    const double omega = k0 * C0;
    double mur = e.coefficient(f, QStringLiteral("mur"));
    if (!std::isfinite(mur) || mur <= 0) mur = 1;
    // ∇×E with e^{-jβz}: (∂y Ez + jβ Ey, -jβ Ex - ∂x Ez, ∂x Ey - ∂y Ex);
    // H = j/(ωµ) ∇×E.
    const cd f0 = J / (omega * Mu0 * mur);
    out.E[0] = ex;
    out.E[1] = ey;
    out.E[2] = ez;
    out.H[0] = f0 * (ezy + J * beta * ey);
    out.H[1] = f0 * (-J * beta * ex - ezx);
    out.H[2] = f0 * curl;
    out.defined = true;
    return out;
}

std::unique_ptr<Problem> makeModeProblem(Solution& solution, int field)
{
    return std::make_unique<ModeProblem>(solution, field);
}

} // namespace qucs_s::fem::detail
