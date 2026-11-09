/*
 * fem3_wave.cpp - light through a 3D model's devices by the finite element
 *                 method (fem3_wave.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_wave.h"

#include "fem3_fe.h"
#include "fem3_geometry.h"
#include "fem_material_plot.h"
#include "fem_results.h"
#include "fem_solver.h"
#include "fem_sparse.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRegularExpression>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace qucs_s::fem {

namespace {

using cd = std::complex<double>;
constexpr double C0 = 299792458.0;
constexpr double Mu0 = 1.25663706212e-6;
const cd J(0, 1);
const double NaN = std::numeric_limits<double>::quiet_NaN();

QString tr(const char* text)
{
    return QCoreApplication::translate("Wave3", text);
}

long long edgeKey(int a, int b)
{
    return (long long)std::min(a, b) * 2147483647LL + std::max(a, b);
}

/// Lagrange's polynomials in ζ of \a order (nodes 0, 1/2, 1 for order 2) and
/// their derivatives; E_z's one order lower (order 2: 1 - ζ and ζ).
void zFunctions(int order, double z, double L[3], double dL[3], double M[2])
{
    if (order == 1) {
        L[0] = 1 - z;
        L[1] = z;
        dL[0] = -1;
        dL[1] = 1;
        M[0] = 1;
    } else {
        L[0] = (1 - z) * (1 - 2 * z);
        L[1] = 4 * z * (1 - z);
        L[2] = z * (2 * z - 1);
        dL[0] = 4 * z - 3;
        dL[1] = 4 - 8 * z;
        dL[2] = 4 * z - 1;
        M[0] = 1 - z;
        M[1] = z;
    }
}

/// Gauss-Legendre's points and weights on [0, 1].
void gauss01(int n, std::vector<double>& x, std::vector<double>& w)
{
    static const double x2[] = {-0.5773502691896257, 0.5773502691896257}, w2[] = {1, 1};
    static const double x3[] = {-0.7745966692414834, 0, 0.7745966692414834}, w3[] = {0.5555555555555556, 0.8888888888888888, 0.5555555555555556};
    static const double x4[] = {-0.8611363115940526, -0.3399810435848563, 0.3399810435848563, 0.8611363115940526},
                        w4[] = {0.3478548451374538, 0.6521451548625461, 0.6521451548625461, 0.3478548451374538};
    const double* px = n <= 2 ? x2 : n == 3 ? x3 : x4;
    const double* pw = n <= 2 ? w2 : n == 3 ? w3 : w4;
    const int m = n <= 2 ? 2 : n == 3 ? 3 : 4;
    x.resize(std::size_t(m));
    w.resize(std::size_t(m));
    for (int i = 0; i < m; ++i) {
        x[std::size_t(i)] = 0.5 * (px[i] + 1);
        w[std::size_t(i)] = 0.5 * pw[i];
    }
}

} // namespace

// ------------------------------------------------------------- the space

EdgeSpace3::EdgeSpace3(std::shared_ptr<const Mesh3> mesh, int order) : a_mesh(std::move(mesh)), a_order(order == 1 ? 1 : 2)
{
    a_nodes = std::make_shared<Space>(a_mesh->plan, a_order);
    a_edges = std::make_shared<EdgeSpace>(a_mesh->plan, a_order);
    a_layers = std::max(0, int(a_mesh->heights.size()) - 1);
    a_levels = a_order == 1 ? a_layers + 1 : 2 * a_layers + 1;
    const Mesh& plan = *a_mesh->plan;
    if (a_order == 2)
        for (int t = 0; t < int(plan.triangles.size()); ++t) {
            const int* nd = a_nodes->dofs(t);
            for (int l = 0; l < 3; ++l) {
                const int p = plan.triangles[std::size_t(t)][std::size_t(l)], q = plan.triangles[std::size_t(t)][std::size_t((l + 1) % 3)];
                a_middle[(unsigned long long)edgeKey(p, q)] = nd[3 + l];
            }
        }
    const int pe = a_edges->perElement(), pn = a_nodes->perElement(), nk = a_order + 1, mc = a_order;
    const int per = perElement();
    a_dofs.assign(std::size_t(elements()) * std::size_t(per), -1);
    for (int e = 0; e < elements(); ++e) {
        const int t = a_mesh->prismTriangle[std::size_t(e)], j = a_mesh->prismLayer[std::size_t(e)];
        const int* ed = a_edges->dofs(t);
        const int* nd = a_nodes->dofs(t);
        int* out = &a_dofs[std::size_t(e) * std::size_t(per)];
        for (int k = 0; k < nk; ++k)
            for (int i = 0; i < pe; ++i) {
                const long long key = (long long)ed[i] * a_levels + levelOf(j, k);
                auto it = a_across.find(key);
                if (it == a_across.end()) it = a_across.emplace(key, a_size++).first;
                out[k * pe + i] = it->second;
            }
        for (int m = 0; m < mc; ++m)
            for (int n = 0; n < pn; ++n) {
                const long long key = ((long long)nd[n] * a_layers + j) * 2 + m;
                auto it = a_along.find(key);
                if (it == a_along.end()) it = a_along.emplace(key, a_size++).first;
                out[pe * nk + m * pn + n] = it->second;
            }
    }
}

void EdgeSpace3::basis(int e, double xi, double eta, double zeta, Basis& b) const
{
    const int t = a_mesh->prismTriangle[std::size_t(e)], j = a_mesh->prismLayer[std::size_t(e)];
    const AffineMap am = affineMap(*a_nodes, t);
    EdgeSpace::Basis eb;
    a_edges->basis(t, am, xi, eta, eb);
    Shape sh;
    shapeAt(a_order, xi, eta, sh);
    const double z0 = z(j), h = z(j + 1) - z0;
    double L[3], dL[3], M[2];
    zFunctions(a_order, zeta, L, dL, M);
    const int pe = eb.count, nk = a_order + 1, mc = a_order;
    b.count = perElement();
    b.det = std::abs(am.det) * h;
    const QPointF p = am.at(xi, eta);
    b.x = Vec3{p.x(), p.y(), z0 + zeta * h};
    for (int k = 0; k < nk; ++k)
        for (int i = 0; i < pe; ++i) {
            const int a = k * pe + i;
            b.v[a][0] = eb.vx[i] * L[k];
            b.v[a][1] = eb.vy[i] * L[k];
            b.v[a][2] = 0;
            b.c[a][0] = -dL[k] / h * eb.vy[i];
            b.c[a][1] = dL[k] / h * eb.vx[i];
            b.c[a][2] = L[k] * eb.curl[i];
        }
    for (int m = 0; m < mc; ++m)
        for (int n = 0; n < sh.count; ++n) {
            const int a = pe * nk + m * sh.count + n;
            const double gx = sh.dxi[n] * am.grad[1][0] + sh.deta[n] * am.grad[2][0];
            const double gy = sh.dxi[n] * am.grad[1][1] + sh.deta[n] * am.grad[2][1];
            b.v[a][0] = 0;
            b.v[a][1] = 0;
            b.v[a][2] = sh.n[n] * M[m];
            b.c[a][0] = M[m] * gy;
            b.c[a][1] = -M[m] * gx;
            b.c[a][2] = 0;
        }
}

std::vector<int> EdgeSpace3::floorDofs(int t, int height) const
{
    std::vector<int> out;
    const int* ed = a_edges->dofs(t);
    for (int i = 0; i < a_edges->perElement(); ++i) {
        auto it = a_across.find((long long)ed[i] * a_levels + levelOf(height, 0));
        if (it != a_across.end()) out.push_back(it->second);
    }
    return out;
}

std::vector<int> EdgeSpace3::wallDofs(int a, int b, int layer) const
{
    std::vector<int> out;
    for (int g : a_edges->edgeDofs(a, b))
        for (int k = 0; k <= a_order; ++k) {
            auto it = a_across.find((long long)g * a_levels + levelOf(layer, k));
            if (it != a_across.end()) out.push_back(it->second);
        }
    std::vector<int> nodes{a, b};
    if (a_order == 2) {
        auto mid = a_middle.find((unsigned long long)edgeKey(a, b));
        if (mid != a_middle.end()) nodes.push_back(mid->second);
    }
    for (int n : nodes)
        for (int m = 0; m < a_order; ++m) {
            auto it = a_along.find(((long long)n * a_layers + layer) * 2 + m);
            if (it != a_along.end()) out.push_back(it->second);
        }
    return out;
}

// ------------------------------------------------------------ materials

cd relativePermittivity(const Node& material, double lambda, const Scope& scope)
{
    MaterialSweep sw;
    sw.variable = QStringLiteral("lambda");
    sw.from = sw.to = lambda;
    sw.points = 1;
    sw.lambda = lambda;
    const MaterialCurves c = materialCurves(material, {QStringLiteral("n"), QStringLiteral("kappa"), QStringLiteral("epsilonr")}, sw, scope);
    double n = NAN, k = 0, er = NAN;
    for (const MaterialCurve& curve : c.curves) {
        if (curve.values.empty() || curve.axis > 0) continue;
        const double v = curve.values.front();
        if (!std::isfinite(v)) continue;
        if (curve.key == QLatin1String("n")) n = v;
        else if (curve.key == QLatin1String("kappa")) k = v;
        else if (curve.key == QLatin1String("epsilonr")) er = v;
    }
    if (std::isfinite(n) && n > 0) return cd(n, -std::max(0.0, k)) * cd(n, -std::max(0.0, k));
    if (std::isfinite(er) && er > 0) return er;
    return 1.0;
}

// --------------------------------------------------------------- the solve

namespace {

/// A port as set up: its plane, its cut's modes, its faces' quadrilaterals
/// and the prism inside each.
struct PortSetup {
    QPointF origin, across, s;   // a point of its plane (geometry unit), its across and into-the-device directions
    EmeModes modes;
    std::vector<int> taken;      // its modes taken (S-parameter ports), by index in modes
    struct Quad {
        int a = -1, b = -1, layer = -1, prism = -1;
    };
    std::vector<Quad> quads;
};

} // namespace

Wave3Result solveWave3(const Model& model, std::shared_ptr<ParameterScope> parameters, const Topology3& topology, std::shared_ptr<const Mesh3> mesh,
                       const Wave3Settings& settings, double frequency)
{
    Wave3Result r;
    r.frequency = frequency;
    QElapsedTimer timer;
    timer.start();
    if (!mesh || mesh->prisms.empty()) {
        r.error = tr("there is no mesh");
        return r;
    }
    if (!parameters) parameters = evaluateParameters(model);
    auto tell = [&](double f, const QString& what) {
        if (settings.progress) settings.progress(f, what);
    };
    auto cancelled = [&] { return settings.cancelled && settings.cancelled(); };
    const double lambda = C0 / frequency, k0 = 2 * M_PI / lambda, omega = 2 * M_PI * frequency;
    const Mesh3& m3 = *mesh;
    const Mesh& plan = *m3.plan;
    const double us = m3.unitScale;
    const int layers = int(m3.heights.size()) - 1;
    auto space = std::make_shared<EdgeSpace3>(mesh, settings.order);
    r.space = space;
    const int N = space->size();
    r.unknowns = N;
    // Each domain's material: the last enabled one that has it.
    std::vector<const Node*> ofDomain(topology.domains.size(), nullptr);
    for (const Node& mat : model.materials().children) {
        if (!mat.enabled || mat.type != QLatin1String("material")) continue;
        QString why;
        for (int d : resolveSelection3(topology, model, Level::Domain, mat.selection(), &why))
            if (d >= 0 && d < int(ofDomain.size())) ofDomain[std::size_t(d)] = &mat;
    }
    std::vector<cd> eps(topology.domains.size(), 1.0);
    for (std::size_t d = 0; d < ofDomain.size(); ++d)
        if (ofDomain[d]) eps[d] = relativePermittivity(*ofDomain[d], lambda, parameters->scope);
    for (std::size_t e = 0; e < m3.prisms.size(); ++e) {
        const int d = m3.domain[e];
        if (d < 0 || d >= int(ofDomain.size()) || !ofDomain[std::size_t(d)]) {
            r.error = tr("a domain has no material (domain %1)").arg(d + 1);
            return r;
        }
    }
    // The mesh against the wavelength in each material: its longest edge a
    // quarter of λ0/n or less for order 2 (a junction's |S21|² 0.8999 with
    // silicon's at a fifth, 0.8995 at an eighth; 0.914 at a half), a tenth
    // for order 1.
    {
        std::vector<double> longest(topology.domains.size(), 0.0);
        for (std::size_t e = 0; e < m3.prisms.size(); ++e) {
            const auto& pr = m3.prisms[e];
            double h = 0;
            for (int a = 0; a < 3; ++a) {
                const Vec3& p = m3.nodes[std::size_t(pr[std::size_t(a)])];
                const Vec3& q = m3.nodes[std::size_t(pr[std::size_t((a + 1) % 3)])];
                const Vec3& t = m3.nodes[std::size_t(pr[std::size_t(a + 3)])];
                h = std::max({h, std::hypot(p.x - q.x, p.y - q.y), std::abs(t.z - p.z)});
            }
            const int d = m3.domain[e];
            if (d >= 0 && d < int(longest.size())) longest[std::size_t(d)] = std::max(longest[std::size_t(d)], h * us);
        }
        const double per = settings.order == 2 ? 4 : 10;
        double worst = 0;
        int at = -1;
        for (std::size_t d = 0; d < longest.size(); ++d) {
            const double n = std::sqrt(std::max(1.0, eps[d].real()));
            const double ratio = longest[d] * n * per / lambda;
            if (ratio > worst) {
                worst = ratio;
                at = int(d);
            }
        }
        if (worst > 1 && at >= 0)
            r.warnings << tr("the mesh is coarse for the wavelength: domain %1's elements reach %2 against λ0/n/%3 = %4 - the S-parameters "
                             "want a finer one there (a size per domain)")
                              .arg(at + 1)
                              .arg(formatQuantity(longest[std::size_t(at)], Dim::length()))
                              .arg(per)
                              .arg(formatQuantity(lambda / std::sqrt(std::max(1.0, eps[std::size_t(at)].real())) / per, Dim::length()));
    }
    // The PMLs: stretched outward from the box of the rest, by axis, as far as
    // each PML domain reaches past it.
    std::set<int> pml(settings.pmlDomains.begin(), settings.pmlDomains.end());
    double inner[6] = {1e300, -1e300, 1e300, -1e300, 1e300, -1e300};
    std::map<int, std::array<double, 6>> pmlBox;
    for (std::size_t e = 0; e < m3.prisms.size(); ++e) {
        const int d = m3.domain[e];
        double* box = nullptr;
        if (pml.count(d)) {
            auto it = pmlBox.find(d);
            if (it == pmlBox.end()) it = pmlBox.emplace(d, std::array<double, 6>{1e300, -1e300, 1e300, -1e300, 1e300, -1e300}).first;
            box = it->second.data();
        } else
            box = inner;
        for (int n : m3.prisms[e]) {
            const Vec3& p = m3.nodes[std::size_t(n)];
            const double c[3] = {p.x * us, p.y * us, p.z * us};
            for (int a = 0; a < 3; ++a) {
                box[2 * a] = std::min(box[2 * a], c[a]);
                box[2 * a + 1] = std::max(box[2 * a + 1], c[a]);
            }
        }
    }
    const double logR = std::log(1 / std::max(settings.pmlReflection, 1e-300));
    // (s along each axis at a point of PML domain d.)
    auto stretch = [&](int d, const Vec3& x, cd s[3]) {
        s[0] = s[1] = s[2] = 1.0;
        auto it = pmlBox.find(d);
        if (it == pmlBox.end()) return;
        const double c[3] = {x.x, x.y, x.z};
        const double k = k0 * std::sqrt(std::max(1.0, eps[std::size_t(d)].real()));
        for (int a = 0; a < 3; ++a) {
            const double lo = inner[2 * a], hi = inner[2 * a + 1];
            double depth = 0, xi = 0;
            if (c[a] > hi && it->second[2 * a + 1] > hi) {
                depth = it->second[2 * a + 1] - hi;
                xi = (c[a] - hi) / depth;
            } else if (c[a] < lo && it->second[2 * a] < lo) {
                depth = lo - it->second[2 * a];
                xi = (lo - c[a]) / depth;
            } else
                continue;
            xi = std::clamp(xi, 0.0, 1.0);
            const double sigma0 = 1.5 * logR / (k * depth);
            s[a] = cd(1.0, -sigma0 * xi * xi);
        }
    };
    // Prisms by plan triangle and layer; triangles by plan edge.
    std::unordered_map<long long, int> prismAt;
    for (std::size_t e = 0; e < m3.prisms.size(); ++e) prismAt[(long long)m3.prismTriangle[e] * layers + m3.prismLayer[e]] = int(e);
    auto prism = [&](int t, int j) {
        if (t < 0 || j < 0 || j >= layers) return -1;
        auto it = prismAt.find((long long)t * layers + j);
        return it == prismAt.end() ? -1 : it->second;
    };
    std::unordered_map<long long, std::vector<int>> trianglesOf;
    for (int t = 0; t < int(plan.triangles.size()); ++t)
        for (int l = 0; l < 3; ++l)
            trianglesOf[edgeKey(plan.triangles[std::size_t(t)][std::size_t(l)], plan.triangles[std::size_t(t)][std::size_t((l + 1) % 3)])].push_back(t);
    // The ports: their faces, their planes, their cuts' modes.
    // (A port's faces on a PML left out - metal behind the PML there, its
    // modes those of the window inside -, unless the ports go through them:
    // the same S of a wire, its modes a thirtieth of the time, the PML's own
    // crowding a cut's about the index searched.)
    std::vector<std::set<int>> facesOf(settings.ports.size());
    std::set<int> portFaces;
    for (std::size_t pi = 0; pi < settings.ports.size(); ++pi) {
        int dropped = 0;
        for (int f : settings.ports[pi].faces) {
            if (f < 0 || f >= int(topology.faces.size())) continue;
            const Topology3::Face& tf = topology.faces[std::size_t(f)];
            if (!settings.portsThroughPmls && (pml.count(tf.back) || pml.count(tf.front))) {
                ++dropped;
                continue;
            }
            facesOf[pi].insert(f);
        }
        if (dropped)
            r.log << tr("Port %1: %2 of its faces on a PML left out (metal behind the PML there)").arg(settings.ports[pi].name).arg(dropped);
        portFaces.insert(facesOf[pi].begin(), facesOf[pi].end());
    }
    std::vector<PortSetup> ports(settings.ports.size());
    for (std::size_t pi = 0; pi < settings.ports.size(); ++pi) {
        const Wave3Port& wp = settings.ports[pi];
        PortSetup& ps = ports[pi];
        const std::set<int>& mine = facesOf[pi];
        double nx = 0, ny = 0;
        for (const Mesh3::Face& f : m3.faces) {
            if (!mine.count(f.face)) continue;
            if (!f.quad) {
                r.error = tr("port %1: a port is a wall - a vertical face -, not a floor").arg(wp.name);
                return r;
            }
            const Mesh::Edge& pe = plan.edges[std::size_t(f.plan)];
            PortSetup::Quad q;
            q.a = pe.a;
            q.b = pe.b;
            q.layer = f.height;
            for (int t : trianglesOf[edgeKey(pe.a, pe.b)])
                if (prism(t, f.height) >= 0) q.prism = prism(t, f.height);
            if (q.prism < 0) continue;
            // (Outward: from the prism inside toward the face.)
            const QPointF A = plan.nodes[std::size_t(pe.a)], B = plan.nodes[std::size_t(pe.b)];
            QPointF n(B.y() - A.y(), -(B.x() - A.x()));
            const auto& tv = plan.triangles[std::size_t(m3.prismTriangle[std::size_t(q.prism)])];
            const QPointF c = (plan.nodes[std::size_t(tv[0])] + plan.nodes[std::size_t(tv[1])] + plan.nodes[std::size_t(tv[2])]) / 3.0;
            if ((A.x() - c.x()) * n.x() + (A.y() - c.y()) * n.y() < 0) n = -n;
            const double len = std::hypot(n.x(), n.y());
            nx += n.x() / len;
            ny += n.y() / len;
            if (ps.quads.empty()) ps.origin = A;
            ps.quads.push_back(q);
        }
        if (ps.quads.empty()) {
            r.error = tr("port %1 is on no face of the mesh").arg(wp.name);
            return r;
        }
        const double nl = std::hypot(nx, ny);
        ps.s = QPointF(-nx / nl, -ny / nl);
        ps.across = QPointF(-ps.s.y(), ps.s.x());
        double u0 = 1e300, u1 = -1e300, z0 = 1e300, z1 = -1e300, off = 0;
        for (const PortSetup::Quad& q : ps.quads)
            for (int node : {q.a, q.b}) {
                const QPointF d = plan.nodes[std::size_t(node)] - ps.origin;
                const double u = d.x() * ps.across.x() + d.y() * ps.across.y();
                off = std::max(off, std::abs(d.x() * ps.s.x() + d.y() * ps.s.y()));
                u0 = std::min(u0, u);
                u1 = std::max(u1, u);
                z0 = std::min(z0, m3.heights[std::size_t(q.layer)]);
                z1 = std::max(z1, m3.heights[std::size_t(q.layer) + 1]);
            }
        if (off > 1e-6 * std::max(1.0, u1 - u0)) {
            r.error = tr("port %1: its faces are not in one plane").arg(wp.name);
            return r;
        }
        QString why;
        const double inward = 1e-6 * std::max(u1 - u0, z1 - z0);
        CrossSection cs = cutAcross(topology, ps.origin + ps.s * inward, ps.across, u0, u1, z0, z1, &why);
        if (!why.isEmpty()) {
            r.error = tr("port %1: %2").arg(wp.name, why);
            return r;
        }
        EmeSettings st;
        st.modes = std::max(1, wp.absorbed);
        st.resolution = settings.portResolution;
        st.order = settings.order;
        st.sides = st.floors = QStringLiteral("pec");
        if (settings.portsThroughPmls) st.pmlDomains = settings.pmlDomains;
        st.pmlReflection = settings.pmlReflection;
        st.cancelled = settings.cancelled;
        ps.modes = solveSectionModes(model, parameters, topology, cs, frequency, st, &why);
        if (!why.isEmpty() || ps.modes.count() == 0) {
            r.error = tr("port %1: %2").arg(wp.name, why.isEmpty() ? tr("no mode found") : why);
            return r;
        }
        // (Signed as one frame sees every port - its lateral axis toward +y,
        // or +x where it lies along x -, so that a straight guide's modes
        // reach its far end as they left: S21 e^-jβL.)
        orientModes(ps.modes, ps.across.y() < -1e-9 || (std::abs(ps.across.y()) <= 1e-9 && ps.across.x() < 0));
        if (ps.modes.count() < st.modes)
            r.warnings << tr("port %1 absorbs %2 modes, not the %3 asked: its cut's mode analysis found no more").arg(wp.name).arg(ps.modes.count()).arg(st.modes);
        for (const QString& l : wp.modes) {
            const int k = ps.modes.find(l);
            if (k < 0) {
                r.error = tr("port %1 has no mode %2").arg(wp.name, l);
                return r;
            }
            ps.taken.push_back(k);
            Wave3Result::PortMode pm;
            pm.port = int(pi);
            pm.label = ps.modes.label(k);
            pm.neff = ps.modes.neff(k);
            pm.te = ps.modes.te[std::size_t(k)];
            r.portModes.push_back(pm);
        }
    }
    if (ports.empty()) {
        r.error = tr("there is no port");
        return r;
    }
    const qint64 portsSolved = timer.elapsed();
    tell(0.1, tr("ports' modes"));
    // Metal on every outer face but the ports': tangential E nought there.
    std::vector<char> fixed(std::size_t(N), 0);
    for (const Mesh3::Face& f : m3.faces) {
        if (portFaces.count(f.face)) continue;
        if (!f.quad) {
            const int below = prism(f.plan, f.height - 1), above = prism(f.plan, f.height);
            if ((below >= 0) == (above >= 0)) continue;
            for (int d : space->floorDofs(f.plan, f.height)) fixed[std::size_t(d)] = 1;
        } else {
            const Mesh::Edge& pe = plan.edges[std::size_t(f.plan)];
            int sides = 0;
            for (int t : trianglesOf[edgeKey(pe.a, pe.b)]) sides += prism(t, f.height) >= 0 ? 1 : 0;
            if (sides != 1) continue;
            for (int d : space->wallDofs(pe.a, pe.b, f.height)) fixed[std::size_t(d)] = 1;
        }
    }
    // The matrix: ∫ µ⁻¹ ∇×W·∇×V - k0² ε W·V, element by element on the
    // machine's threads, each part's triplets.
    const int per = space->perElement();
    const PrismRule& rule = settings.order == 1 ? prismRule(2, 2) : prismRule(4, 3);
    const int E = space->elements();
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 32);
    std::vector<std::vector<Eigen::Triplet<cd>>> parts(static_cast<std::size_t>(threads));
    std::atomic<int> nextChunk{0};
    const int chunk = 256;
    auto assemble = [&](int part) {
        EdgeSpace3::Basis b;
        Eigen::MatrixXcd Ke(per, per);
        auto& out = parts[std::size_t(part)];
        for (int c0 = nextChunk++ * chunk; c0 < E; c0 = nextChunk++ * chunk) {
            for (int e = c0; e < std::min(E, c0 + chunk); ++e) {
                const int d = m3.domain[std::size_t(e)];
                const cd er = eps[std::size_t(d)];
                const bool inPml = pml.count(d) != 0;
                Ke.setZero();
                for (const auto& q : rule.points) {
                    space->basis(e, q[0], q[1], q[2], b);
                    cd epsT[3] = {er, er, er}, muInv[3] = {1.0, 1.0, 1.0};
                    if (inPml) {
                        cd s[3];
                        stretch(d, b.x, s);
                        const cd L[3] = {s[1] * s[2] / s[0], s[0] * s[2] / s[1], s[0] * s[1] / s[2]};
                        for (int a = 0; a < 3; ++a) {
                            epsT[a] = er * L[a];
                            muInv[a] = 1.0 / L[a];
                        }
                    }
                    const double w = q[3] * b.det;
                    for (int i = 0; i < per; ++i)
                        for (int k = 0; k <= i; ++k) {
                            cd v = 0;
                            for (int a = 0; a < 3; ++a) v += b.c[i][a] * muInv[a] * b.c[k][a] - k0 * k0 * b.v[i][a] * epsT[a] * b.v[k][a];
                            Ke(i, k) += w * v;
                        }
                }
                const int* dofs = space->dofs(e);
                for (int i = 0; i < per; ++i) {
                    if (fixed[std::size_t(dofs[i])]) continue;
                    for (int k = 0; k < per; ++k) {
                        if (fixed[std::size_t(dofs[k])]) continue;
                        const cd v = k <= i ? Ke(i, k) : Ke(k, i);
                        if (v != cd(0)) out.emplace_back(dofs[i], dofs[k], v);
                    }
                }
            }
            if (cancelled()) return;
        }
    };
    {
        std::vector<std::thread> pool;
        for (int t = 1; t < threads; ++t) pool.emplace_back(assemble, t);
        assemble(0);
        for (std::thread& t : pool) t.join();
    }
    if (cancelled()) {
        r.error = tr("cancelled");
        return r;
    }
    Eigen::SparseMatrix<cd> K(N, N);
    {
        std::size_t total = 0;
        for (const auto& p : parts) total += p.size();
        std::vector<Eigen::Triplet<cd>> all;
        all.reserve(total + std::size_t(N));
        for (auto& p : parts) {
            all.insert(all.end(), p.begin(), p.end());
            std::vector<Eigen::Triplet<cd>>().swap(p);
        }
        for (int i = 0; i < N; ++i)
            if (fixed[std::size_t(i)]) all.emplace_back(i, i, cd(1));
        K.setFromTriplets(all.begin(), all.end());
    }
    K.makeCompressed();
    const qint64 assembled = timer.elapsed();
    tell(0.3, tr("factorizing %1 unknowns").arg(N));
    // The ports' modes on their faces: u_m(W) = ∮ (ŝ × h_m)·W dS for each
    // function W, each port's modes a column.
    int Mtot = 0;
    std::vector<int> firstColumn;
    for (const PortSetup& ps : ports) {
        firstColumn.push_back(Mtot);
        Mtot += ps.modes.count();
    }
    std::vector<std::vector<std::pair<int, cd>>> U(static_cast<std::size_t>(Mtot));   // sparse columns
    for (std::size_t pi = 0; pi < ports.size(); ++pi) {
        const PortSetup& ps = ports[pi];
        const int M = ps.modes.count();
        std::vector<std::unordered_map<int, cd>> cols(static_cast<std::size_t>(M));
        Locator loc(ps.modes.solution->mesh());
        std::vector<double> gx, gw;
        gauss01(settings.order + 2, gx, gw);
        EdgeSpace3::Basis b;
        Eigen::MatrixXcd F;
        const Vec3 s3{ps.s.x(), ps.s.y(), 0}, a3{ps.across.x(), ps.across.y(), 0};
        for (const PortSetup::Quad& q : ps.quads) {
            const int t = m3.prismTriangle[std::size_t(q.prism)];
            const AffineMap am = affineMap(space->planNodes(), t);
            const QPointF A = plan.nodes[std::size_t(q.a)], B = plan.nodes[std::size_t(q.b)];
            const double h = space->z(q.layer + 1) - space->z(q.layer), len = std::hypot(B.x() - A.x(), B.y() - A.y()) * us;
            const double det = (am.p1.x() - am.p0.x()) * (am.p2.y() - am.p0.y()) - (am.p2.x() - am.p0.x()) * (am.p1.y() - am.p0.y());
            const int* dofs = space->dofs(q.prism);
            for (std::size_t it = 0; it < gx.size(); ++it)
                for (std::size_t iz = 0; iz < gx.size(); ++iz) {
                    const QPointF P = (A + (B - A) * gx[it]) * us;   // m
                    const double dx = P.x() - am.p0.x(), dy = P.y() - am.p0.y();
                    const double xi = ((am.p2.y() - am.p0.y()) * dx - (am.p2.x() - am.p0.x()) * dy) / det;
                    const double eta = (-(am.p1.y() - am.p0.y()) * dx + (am.p1.x() - am.p0.x()) * dy) / det;
                    space->basis(q.prism, xi, eta, gx[iz], b);
                    const QPointF rel = A + (B - A) * gx[it] - ps.origin;
                    const double u = rel.x() * ps.across.x() + rel.y() * ps.across.y();
                    const double v = (b.x.z) / us;
                    int tt = -1;
                    double txi = 0, teta = 0;
                    if (!loc.locate(QPointF(u, v), &tt, &txi, &teta)) continue;
                    ps.modes.fieldsAt(tt, txi, teta, F);
                    const double w = gw[it] * gw[iz] * len * h;
                    for (int m = 0; m < M; ++m) {
                        // h in 3D, then ŝ × h.
                        const cd hx = F(3, m) * a3.x + F(5, m) * s3.x, hy = F(3, m) * a3.y + F(5, m) * s3.y, hz = F(4, m);
                        const cd sxh[3] = {s3.y * hz - 0.0 * hy, 0.0 * hx - s3.x * hz, s3.x * hy - s3.y * hx};
                        for (int i = 0; i < per; ++i) {
                            if (fixed[std::size_t(dofs[i])]) continue;
                            const cd val = w * (sxh[0] * b.v[i][0] + sxh[1] * b.v[i][1] + sxh[2] * b.v[i][2]);
                            if (val != cd(0)) cols[std::size_t(m)][dofs[i]] += val;
                        }
                    }
                }
        }
        for (int m = 0; m < M; ++m)
            for (const auto& [dof, val] : cols[std::size_t(m)]) U[std::size_t(firstColumn[pi] + m)].push_back({dof, val});
    }
    // K factorized once; G = Uᵀ K⁻¹ U, a solve a column.
    SparseComplexLdlt lu;
    lu.setMemoryLimit(settings.memoryLimit);
    lu.compute(K);
    if (lu.info() != Eigen::Success) {
        r.error = tr("the system could not be factorized (%1)").arg(QString::fromStdString(lu.lastErrorMessage()));
        return r;
    }
    const qint64 factorized = timer.elapsed();
    auto dense = [&](int col) {
        Eigen::VectorXcd v = Eigen::VectorXcd::Zero(N);
        for (const auto& [dof, val] : U[std::size_t(col)]) v(dof) = val;
        return v;
    };
    auto project = [&](const Eigen::VectorXcd& x, Eigen::VectorXcd& out) {
        out.resize(Mtot);
        for (int c = 0; c < Mtot; ++c) {
            cd s = 0;
            for (const auto& [dof, val] : U[std::size_t(c)]) s += val * x(dof);
            out(c) = s;
        }
    };
    // (The columns solved together, a block of 16 at a time: the factors read
    // once a block, a Cancel heard between blocks.)
    Eigen::MatrixXcd G(Mtot, Mtot);
    {
        Eigen::VectorXcd col;
        const int block = 16;
        for (int c0 = 0; c0 < Mtot; c0 += block) {
            if (cancelled()) {
                r.error = tr("cancelled");
                return r;
            }
            const int nb = std::min(block, Mtot - c0);
            Eigen::MatrixXcd B = Eigen::MatrixXcd::Zero(N, nb);
            for (int k = 0; k < nb; ++k)
                for (const auto& [dof, val] : U[std::size_t(c0 + k)]) B(dof, k) = val;
            const Eigen::MatrixXcd Y = lu.solveMany(B);
            for (int k = 0; k < nb; ++k) {
                project(Y.col(k), col);
                G.col(c0 + k) = col;
            }
            tell(0.3 + 0.6 * (c0 + nb) / Mtot, tr("ports' modes %1 of %2").arg(c0 + nb).arg(Mtot));
        }
    }
    // S over every port mode: (D⁻¹ + G)⁻¹ (G - D⁻¹), D = jωµ0.
    const cd Dinv = 1.0 / (J * omega * Mu0);
    const Eigen::MatrixXcd A = G + Dinv * Eigen::MatrixXcd::Identity(Mtot, Mtot);
    const Eigen::PartialPivLU<Eigen::MatrixXcd> Alu(A);
    const Eigen::MatrixXcd Sall = Alu.solve(G - Dinv * Eigen::MatrixXcd::Identity(Mtot, Mtot));
    // The port modes taken: rows and columns.
    std::vector<int> index;   // by port mode: its column in Sall
    for (std::size_t pi = 0; pi < ports.size(); ++pi)
        for (int k : ports[pi].taken) index.push_back(firstColumn[pi] + k);
    const int P = int(index.size());
    r.S = Eigen::MatrixXcd::Zero(P, P);
    for (int i = 0; i < P; ++i)
        for (int j = 0; j < P; ++j) r.S(i, j) = Sall(index[std::size_t(i)], index[std::size_t(j)]);
    // What leaves by every port in every mode carrying power - propagating,
    // its loss a thousandth of its phase or less: its |S|² its power (W of
    // 1 W fed; near it where the guide is lossy) -, those taken or not.
    std::vector<char> carries(static_cast<std::size_t>(Mtot), 0);
    for (std::size_t pi = 0; pi < ports.size(); ++pi)
        for (int k = 0; k < ports[pi].modes.count(); ++k) {
            const cd b = ports[pi].modes.beta[std::size_t(k)];
            carries[std::size_t(firstColumn[pi] + k)] = b.real() > 0 && std::abs(b.imag()) <= 1e-3 * b.real() ? 1 : 0;
        }
    r.balance.assign(std::size_t(P), 0.0);
    for (int j = 0; j < P; ++j)
        for (int i = 0; i < Mtot; ++i)
            if (carries[std::size_t(i)]) r.balance[std::size_t(j)] += std::norm(Sall(i, index[std::size_t(j)]));
    // Each fed port mode's field: K E = f - U c, f = -2jωµ0 u_p, c = D... (the
    // ports' modes' coupling): E = K⁻¹ (f - U c) with c = (D⁻¹ + G)⁻¹ Uᵀ K⁻¹ f.
    {
        int at = 0;
        for (std::size_t pi = 0; pi < ports.size(); ++pi)
            for (std::size_t k = 0; k < ports[pi].taken.size(); ++k, ++at) {
                if (!settings.ports[pi].excite) continue;
                if (settings.fieldsKept >= 0 && int(r.fields.size()) >= settings.fieldsKept) continue;
                const int col = index[std::size_t(at)];
                const cd scale = -2.0 * J * omega * Mu0;
                const Eigen::VectorXcd UtKf = scale * G.col(col);
                const Eigen::VectorXcd c = Alu.solve(UtKf);
                Eigen::VectorXcd rhs = scale * dense(col);
                for (int cc = 0; cc < Mtot; ++cc)
                    for (const auto& [dof, val] : U[std::size_t(cc)]) rhs(dof) -= val * c(cc);
                r.fields.push_back(lu.solve(rhs));
                r.fed.push_back(at);
            }
    }
    r.log << tr("Full-wave solve at %1: %2 unknowns (order %3), %4 prisms, %5 port modes absorbed; %6 s - the ports' modes %12 s, assembly %7 s, "
                "%8 %9 s, the ports' %10 solves %11 s")
                 .arg(formatQuantity(lambda, Dim::length()))
                 .arg(N)
                 .arg(settings.order)
                 .arg(E)
                 .arg(Mtot)
                 .arg(timer.elapsed() / 1000.0, 0, 'f', 2)
                 .arg((assembled - portsSolved) / 1000.0, 0, 'f', 2)
                 .arg(lu.name())
                 .arg((factorized - assembled) / 1000.0, 0, 'f', 2)
                 .arg(Mtot)
                 .arg((timer.elapsed() - factorized) / 1000.0, 0, 'f', 2)
                 .arg(portsSolved / 1000.0, 0, 'f', 2);
    if (lu.factorBytes() > 0) r.log << tr("Its factors and their workspace %1, as the analysis said").arg(bytesText(lu.factorBytes()));
    tell(1.0, tr("done"));
    return r;
}

bool readWave3Settings(const Model& model, const Node& physics, const Topology3& topology, const ParameterScope& parameters, Wave3Settings* out,
                       QString* error)
{
    QStringList problems;
    Wave3Settings& st = *out;
    st = Wave3Settings();
    st.order = physics.text(QStringLiteral("order")) == QLatin1String("1") ? 1 : 2;
    auto number = [&](const Node& n, const QString& key, double fallback, const QString& what) {
        const QString text = n.text(key).trimmed();
        if (text.isEmpty()) return fallback;
        const Expression e = Expression::compile(text, parameters.scope);
        const double v = e.isValid() && e.isConstant() ? e.constant() : NaN;
        if (!std::isfinite(v)) problems << tr("%1: %2 is a number, not %3").arg(n.name(), what, text);
        return v;
    };
    st.portResolution = number(physics, QStringLiteral("portresolution"), 30, tr("its ports' elements per wavelength"));
    if (!(st.portResolution >= 4)) problems << tr("%1: its ports' elements per wavelength four at least").arg(physics.name());
    st.portsThroughPmls = physics.flag(QStringLiteral("throughpml"));
    // Its PMLs: their domains, the strongest reflection asked of them.
    std::set<int> pmls;
    double reflection = 0;
    for (const Node& c : physics.children) {
        if (!c.enabled || c.type != QLatin1String("waves3/pml")) continue;
        QString why;
        const QVector<int> picked = resolveSelection3(topology, model, Level::Domain, c.selection(), &why);
        if (!why.isEmpty()) problems << tr("%1: %2").arg(c.name(), why);
        if (picked.isEmpty()) problems << tr("%1 has no domain").arg(c.name());
        for (int d : picked) pmls.insert(d);
        const double R = number(c, QStringLiteral("reflection"), 1e-6, tr("its reflection"));
        if (!(R > 0 && R < 1)) problems << tr("%1: its reflection is a number between 0 and 1 (1e-6: e^-13.8)").arg(c.name());
        else reflection = std::max(reflection, R);
    }
    st.pmlDomains.assign(pmls.begin(), pmls.end());
    if (reflection > 0) st.pmlReflection = reflection;
    // Its ports: their faces, modes, how many they absorb.
    QSet<QString> names;
    for (const Node& c : physics.children) {
        if (!c.enabled || c.type != QLatin1String("waves3/port")) continue;
        Wave3Port p;
        p.name = c.text(QStringLiteral("name")).trimmed();
        if (p.name.isEmpty()) problems << tr("%1: a port without a name").arg(c.name());
        if (names.contains(p.name)) problems << tr("two ports named %1: each its own").arg(p.name);
        names.insert(p.name);
        QString why;
        for (int f : resolveSelection3(topology, model, Level::Boundary, c.selection(), &why)) p.faces.push_back(f);
        if (!why.isEmpty()) problems << tr("port %1: %2").arg(p.name, why);
        if (p.faces.empty()) problems << tr("port %1 is on no face: select the end of its waveguide").arg(p.name);
        for (int f : p.faces)
            if (f >= 0 && f < int(topology.faces.size()) && !topology.faces[std::size_t(f)].exterior()) {
                problems << tr("port %1: a face inside the geometry - a port is on its outside, a waveguide's end").arg(p.name);
                break;
            }
        p.modes.clear();
        for (const QString& m : c.text(QStringLiteral("modes")).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString t = m.trimmed().toUpper();
            static const QRegularExpression re(QStringLiteral("^(TE|TM)\\d+$|^\\d+$"));
            if (!re.match(t).hasMatch()) problems << tr("port %1: a mode is TE0, TM1... or a number from 1, not %2").arg(p.name, m.trimmed());
            else p.modes << t;
        }
        if (p.modes.isEmpty()) problems << tr("port %1 takes no mode").arg(p.name);
        p.absorbed = std::clamp(c.integer(QStringLiteral("absorbed")), 1, 100);
        if (p.absorbed < int(p.modes.size())) p.absorbed = int(p.modes.size());
        p.excite = c.flag(QStringLiteral("excite"));
        st.ports.push_back(p);
    }
    if (st.ports.empty()) problems << tr("%1 has no Optical Port").arg(physics.name());
    if (!problems.isEmpty()) {
        *error = problems.join(QLatin1String("; "));
        return false;
    }
    return true;
}

std::vector<std::array<std::complex<double>, 3>> nodalField(const EdgeSpace3& space, const Eigen::VectorXcd& field, const Space3& nodes)
{
    std::vector<std::array<cd, 3>> out(std::size_t(nodes.size()), std::array<cd, 3>{0.0, 0.0, 0.0});
    std::vector<int> count(std::size_t(nodes.size()), 0);
    if (field.size() != space.size() || nodes.order() != 1) return out;
    // (A prism's corners: its plan triangle's at ζ 0 and 1, as Space3 numbers
    // them.)
    static const double corner[6][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}};
    EdgeSpace3::Basis b;
    const int per = space.perElement();
    for (int e = 0; e < space.elements(); ++e) {
        const int* dofs = space.dofs(e);
        const int* nd = nodes.dofs(e);
        for (int k = 0; k < 6; ++k) {
            space.basis(e, corner[k][0], corner[k][1], corner[k][2], b);
            cd E[3] = {0.0, 0.0, 0.0};
            for (int i = 0; i < per; ++i) {
                const cd c = field(dofs[i]);
                if (c == cd(0)) continue;
                for (int a = 0; a < 3; ++a) E[a] += c * b.v[i][a];
            }
            auto& o = out[std::size_t(nd[k])];
            for (int a = 0; a < 3; ++a) o[std::size_t(a)] += E[a];
            ++count[std::size_t(nd[k])];
        }
    }
    for (std::size_t i = 0; i < out.size(); ++i)
        if (count[i] > 0)
            for (cd& v : out[i]) v /= double(count[i]);
    return out;
}

Estimate3 estimateWave3(std::shared_ptr<const Mesh3> mesh, int order)
{
    Estimate3 e;
    e.order = order;
    e.method = QStringLiteral("direct");
    if (!mesh || mesh->prisms.empty()) return e;
    const EdgeSpace3 space(mesh, order);
    e.unknowns = space.size();
    e.elements = space.elements();
    e.perElement = space.perElement();
    // (Its nonzeros some 60 a row of order 2, 20 of order 1 - not counted.)
    e.nonzeros = qint64(e.unknowns) * (order == 2 ? 60 : 20);
    const double n = double(e.unknowns);
    e.directBytes = 2.0e4 * n;
    e.directSeconds = 2.8e-10 * std::pow(n, 1.9) + 1e-6 * n;
    e.iterativeBytes = e.directBytes;
    e.iterativeSeconds = e.directSeconds;
    return e;
}

} // namespace qucs_s::fem
