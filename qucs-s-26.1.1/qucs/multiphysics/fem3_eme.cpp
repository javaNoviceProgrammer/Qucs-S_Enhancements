/*
 * fem3_eme.cpp - light through a 3D model's waveguides by eigenmode
 *                expansion (fem3_eme.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_eme.h"

#include "fem_edge.h"
#include "fem_fe.h"
#include "fem_material_plot.h"
#include "fem_mesh.h"
#include "fem_results.h"
#include "fem_solver.h"

#include <Eigen/Dense>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <thread>

namespace qucs_s::fem {

namespace {

using cd = std::complex<double>;
constexpr double C0 = 299792458.0;
constexpr double Mu0 = 1.25663706127e-6;
const cd J(0, 1);
const double NaN = std::numeric_limits<double>::quiet_NaN();

QString tr(const char* text)
{
    return QCoreApplication::translate("Eme3", text);
}

QString number(double v)
{
    return QString::number(v, 'g', 17);
}

// ------------------------------------------------------------------ the cut

/// Where the line O + u A crosses a straight piece from p0 to p1 (N across
/// the line): its u, into \a out. A piece along the line gives both ends.
void crossStraight(QPointF p0, QPointF p1, QPointF O, QPointF A, QPointF N, std::vector<double>& out)
{
    auto dist = [&](QPointF p) { return (p.x() - O.x()) * N.x() + (p.y() - O.y()) * N.y(); };
    auto along = [&](QPointF p) { return (p.x() - O.x()) * A.x() + (p.y() - O.y()) * A.y(); };
    const double d0 = dist(p0), d1 = dist(p1);
    if (d0 == 0 && d1 == 0) {
        out.push_back(along(p0));
        out.push_back(along(p1));
        return;
    }
    if ((d0 < 0 && d1 < 0) || (d0 > 0 && d1 > 0)) return;
    const double w = d0 / (d0 - d1);
    out.push_back(along(p0 + (p1 - p0) * w));
}

/// Where the line crosses plan segment \a seg (on its curve: a line, an arc
/// of an ellipse, a polyline), its u into \a out.
void crossSegment(const Topology& plan, const Topology::Segment& seg, QPointF O, QPointF A, QPointF N, std::vector<double>& out)
{
    const QPointF p0 = plan.vertices[std::size_t(seg.v0)], p1 = plan.vertices[std::size_t(seg.v1)];
    if (seg.curve < 0 || seg.curve >= int(plan.curves.size())) return crossStraight(p0, p1, O, A, N, out);
    const Curve& c = plan.curves[std::size_t(seg.curve)];
    const double lo = std::min(seg.t0, seg.t1), hi = std::max(seg.t0, seg.t1);
    switch (c.kind) {
    case Curve::Line: crossStraight(p0, p1, O, A, N, out); return;
    case Curve::Polyline:
        for (int i = int(std::floor(lo)); i < int(std::ceil(hi)); ++i) {
            const double a = std::max(lo, double(i)), b = std::min(hi, double(i + 1));
            if (b > a) crossStraight(c.at(a), c.at(b), O, A, N, out);
        }
        return;
    case Curve::Arc: {
        // (c + a cos t + b sin t - O)·N = k + R cos(t - φ) = 0.
        const double an = c.a.x() * N.x() + c.a.y() * N.y(), bn = c.b.x() * N.x() + c.b.y() * N.y();
        const double k = (c.c.x() - O.x()) * N.x() + (c.c.y() - O.y()) * N.y();
        const double R = std::hypot(an, bn);
        if (!(R > 0)) return;
        const double q = -k / R;
        if (q < -1 - 1e-12 || q > 1 + 1e-12) return;
        const double phi = std::atan2(bn, an), alpha = std::acos(std::clamp(q, -1.0, 1.0));
        const double eps = 1e-12 * std::max(1.0, std::abs(hi - lo));
        for (double t : {phi + alpha, phi - alpha}) {
            // (Into [lo, hi] by whole turns.)
            const double tt = t + 2 * M_PI * std::ceil((lo - eps - t) / (2 * M_PI));
            if (tt > hi + eps) continue;
            const QPointF p = c.at(tt);
            out.push_back((p.x() - O.x()) * A.x() + (p.y() - O.y()) * A.y());
        }
        return;
    }
    }
}

bool sameRect(const CrossSection::Rect& a, const CrossSection::Rect& b, double tol, const std::vector<int>* classes)
{
    auto classOf = [&](int d) { return classes && d >= 0 && d < int(classes->size()) ? (*classes)[std::size_t(d)] : d; };
    return classOf(a.domain) == classOf(b.domain) && std::abs(a.u0 - b.u0) <= tol && std::abs(a.u1 - b.u1) <= tol
           && std::abs(a.z0 - b.z0) <= tol && std::abs(a.z1 - b.z1) <= tol;
}

} // namespace

bool CrossSection::sameAs(const CrossSection& o, double tolerance, const std::vector<int>* classes) const
{
    if (rects.size() != o.rects.size()) return false;
    // (A bend's cut is not a straight one's, nor another radius's.)
    if (std::abs(bendRadius - o.bendRadius) > tolerance || std::abs(bendCenter - o.bendCenter) > tolerance) return false;
    if (std::abs(u0 - o.u0) > tolerance || std::abs(u1 - o.u1) > tolerance || std::abs(z0 - o.z0) > tolerance || std::abs(z1 - o.z1) > tolerance)
        return false;
    for (std::size_t i = 0; i < rects.size(); ++i)
        if (!sameRect(rects[i], o.rects[i], tolerance, classes)) return false;
    return true;
}

CrossSection cutAcross(const Topology3& topology, QPointF origin, QPointF across, double u0, double u1, double z0, double z1, QString* error)
{
    CrossSection cs;
    cs.u0 = u0;
    cs.u1 = u1;
    cs.z0 = z0;
    cs.z1 = z1;
    if (!topology.plan || !(u1 > u0) || !(z1 > z0)) {
        if (error) *error = tr("the window of the cut is empty");
        return cs;
    }
    const Topology& plan = *topology.plan;
    const QPointF N(across.y(), -across.x());
    std::vector<double> us{u0, u1};
    for (const Topology::Segment& seg : plan.segments) crossSegment(plan, seg, origin, across, N, us);
    const double scale = std::max({1.0, std::abs(u0), std::abs(u1), u1 - u0});
    const double tol = 1e-9 * scale;
    std::sort(us.begin(), us.end());
    std::vector<double> cuts;
    for (double u : us) {
        if (u < u0 - tol || u > u1 + tol) continue;
        const double v = std::clamp(u, u0, u1);
        if (cuts.empty() || v - cuts.back() > tol) cuts.push_back(v);
    }
    // Each interval's plan domain at its middle, through each slab.
    const int slabs = topology.slabs();
    std::vector<std::vector<CrossSection::Rect>> bySlab(std::size_t(std::max(slabs, 0)));
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        const double ua = cuts[i], ub = cuts[i + 1];
        if (ub - ua <= tol) continue;
        const double um = (ua + ub) / 2;
        const int d = plan.domainAt(origin + across * um);
        for (int k = 0; k < slabs; ++k) {
            const double lo = std::max(z0, topology.levels[std::size_t(k)]), hi = std::min(z1, topology.levels[std::size_t(k) + 1]);
            if (hi - lo <= 1e-9 * std::max(1.0, std::abs(hi) + std::abs(lo))) continue;
            const int D = d >= 0 ? topology.cell(d, k) : -1;
            if (D < 0) {
                if (error)
                    *error = tr("the cut's window reaches outside the solids at u = %1, z = %2 (the geometry's unit): give it a window "
                                "inside them, or a solid round it (the cladding)")
                                 .arg(formatNumber(um, 6), formatNumber((lo + hi) / 2, 6));
                cs.rects.clear();
                return cs;
            }
            std::vector<CrossSection::Rect>& row = bySlab[std::size_t(k)];
            if (!row.empty() && row.back().domain == D && std::abs(row.back().u1 - ua) <= tol) row.back().u1 = ub;
            else row.push_back({ua, ub, lo, hi, D});
        }
    }
    // Rectangles of the same domain and the same width one above the other
    // made one.
    std::vector<CrossSection::Rect> all;
    for (const auto& row : bySlab) all.insert(all.end(), row.begin(), row.end());
    std::sort(all.begin(), all.end(), [](const CrossSection::Rect& a, const CrossSection::Rect& b) {
        if (a.u0 != b.u0) return a.u0 < b.u0;
        if (a.u1 != b.u1) return a.u1 < b.u1;
        return a.z0 < b.z0;
    });
    for (const CrossSection::Rect& r : all) {
        if (!cs.rects.empty()) {
            CrossSection::Rect& last = cs.rects.back();
            if (last.domain == r.domain && std::abs(last.u0 - r.u0) <= tol && std::abs(last.u1 - r.u1) <= tol && std::abs(last.z1 - r.z0) <= tol) {
                last.z1 = r.z1;
                continue;
            }
        }
        cs.rects.push_back(r);
    }
    std::sort(cs.rects.begin(), cs.rects.end(), [](const CrossSection::Rect& a, const CrossSection::Rect& b) {
        if (a.z0 != b.z0) return a.z0 < b.z0;
        return a.u0 < b.u0;
    });
    if (cs.rects.empty() && error) *error = tr("the cut crosses no solid");
    return cs;
}

// ---------------------------------------------------------------- the modes

namespace {

/// A mode's label among the window's modes \a in: TE or TM by its TE
/// fraction, numbered in order of index within its polarization (the modes
/// in the order given: by real index, falling).
QString labelOf(const std::vector<double>& te, const std::vector<char>& in, int m)
{
    if (!in[std::size_t(m)]) return QStringLiteral("PML%1").arg(m + 1);
    const bool isTe = te[std::size_t(m)] >= 0.5;
    int k = 0;
    for (int i = 0; i < m; ++i)
        if (in[std::size_t(i)] && (te[std::size_t(i)] >= 0.5) == isTe) ++k;
    return (isTe ? QStringLiteral("TE") : QStringLiteral("TM")) + QString::number(k);
}

/// A triangle's corners (m) and the inverse of its map: (xi, eta) of a point.
struct TriangleMap {
    double x0, y0, a, b, c, d;   // [x1-x0, x2-x0; y1-y0, y2-y0]
    double ia, ib, ic, id;       // its inverse
    double area;                 // signed, m²
    TriangleMap(const Mesh& mesh, int t)
    {
        const auto& tv = mesh.triangles[std::size_t(t)];
        const QPointF p0 = mesh.nodes[std::size_t(tv[0])] * mesh.unitScale, p1 = mesh.nodes[std::size_t(tv[1])] * mesh.unitScale,
                      p2 = mesh.nodes[std::size_t(tv[2])] * mesh.unitScale;
        x0 = p0.x();
        y0 = p0.y();
        a = p1.x() - x0;
        b = p2.x() - x0;
        c = p1.y() - y0;
        d = p2.y() - y0;
        const double det = a * d - b * c;
        area = det / 2;
        ia = d / det;
        ib = -b / det;
        ic = -c / det;
        id = a / det;
    }
    void reference(double x, double y, double* xi, double* eta) const
    {
        const double dx = x - x0, dy = y - y0;
        *xi = ia * dx + ib * dy;
        *eta = ic * dx + id * dy;
    }
};

/// µ along u, v and s of mode set \a m at reference point (xi, eta) of
/// triangle t: in a PML stretched as the mode solver stretched it, in a
/// bend times f, f, 1/f (f = ρ/R) - the mode solver's own.
void modeMu(const EmeModes& m, int t, const AffineMap& am, double xi, double eta, cd mu3[3])
{
    const double mu = t < int(m.mur.size()) && m.mur[std::size_t(t)] > 0 ? m.mur[std::size_t(t)] : 1.0;
    cd muu = mu, muv = mu, mus = mu;
    if (t < int(m.layerOf.size()) && m.layerOf[std::size_t(t)] >= 0) {
        const EmeModes::Layer& L = m.layers[std::size_t(m.layerOf[std::size_t(t)])];
        const QPointF p = am.at(xi, eta);
        auto stretch = [&](double v, int axis) -> cd {
            const double lo = m.inner[2 * axis], hi = m.inner[2 * axis + 1];
            const double outLo = L.outer[2 * axis], outHi = L.outer[2 * axis + 1];
            double depth = 0, x = 0;
            if (v > hi && outHi > hi) {
                depth = outHi - hi;
                x = (v - hi) / depth;
            } else if (v < lo && outLo < lo) {
                depth = lo - outLo;
                x = (lo - v) / depth;
            } else
                return 1.0;
            x = std::clamp(x, 0.0, 1.0);
            const double sigma0 = 1.5 * m.pmlLog / (L.k * depth);
            return cd(1.0, -sigma0 * x * x);
        };
        const cd su = stretch(p.x(), 0), sv = stretch(p.y(), 1);
        muu = mu * sv / su;
        muv = mu * su / sv;
        mus = mu * su * sv;
    }
    if (m.bendRadius > 0) {
        const double f = std::max(std::abs(am.at(xi, eta).x() - m.bendCenter), 1e-300) / m.bendRadius;
        muu *= f;
        muv *= f;
        mus /= f;
    }
    mu3[0] = muu;
    mu3[1] = muv;
    mu3[2] = mus;
}

/// The four transverse fields an overlap reads - e_u, e_v, h_u, h_v - of
/// every mode at points, a row each (row-major: a point's row is together).
using RowMatrix = Eigen::Matrix<cd, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
struct Transverse {
    RowMatrix eu, ev, hu, hv;
    void reserve(int rows, int M)
    {
        if (eu.rows() >= rows && eu.cols() == M) return;
        const int r = std::max(rows, int(eu.rows()));
        eu.resize(r, M);
        ev.resize(r, M);
        hu.resize(r, M);
        hv.resize(r, M);
    }
};

/// transverseAt()'s scratch, kept across its calls on one thread: nothing
/// allocated per point.
struct TransverseScratch {
    Eigen::MatrixXcd et, es;             // the triangle's edge and node coefficients, dof by mode
    Eigen::MatrixXd vx, vy, gx, gy;      // the points' edge basis and E_s's gradient basis
    RowMatrix gu, gz;                    // the points' ∂u E_s and ∂v E_s
    Eigen::RowVectorXcd jb;              // jβ of each mode
};

/// Rows [row, row + n) of \a out: the transverse fields of every mode of \a m
/// at n reference points of triangle t - the triangle's coefficients gathered
/// once, the points' fields small products of them (fieldsAt()'s rows 0, 1,
/// 3 and 4, E_s and h_s not wanted).
void transverseAt(const EmeModes& m, int t, const double* xi, const double* eta, int n, Transverse& out, int row, TransverseScratch& s)
{
    const Field& f = m.solution->fields().front();
    const EdgeSpace& es = *f.edges;
    const Space& ns = *f.space;
    const AffineMap am = affineMap(ns, t);
    const int M = m.count();
    EdgeSpace::Basis b;
    Shape sh;
    for (int p = 0; p < n; ++p) {
        es.basis(t, am, xi[p], eta[p], b);
        shapeAt(ns.order(), xi[p], eta[p], sh);
        if (p == 0) {
            s.vx.resize(n, b.count);
            s.vy.resize(n, b.count);
            s.gx.resize(n, sh.count);
            s.gy.resize(n, sh.count);
        }
        for (int i = 0; i < b.count; ++i) {
            s.vx(p, i) = b.vx[i];
            s.vy(p, i) = b.vy[i];
        }
        for (int i = 0; i < sh.count; ++i) {
            s.gx(p, i) = sh.dxi[i] * am.grad[1][0] + sh.deta[i] * am.grad[2][0];
            s.gy(p, i) = sh.dxi[i] * am.grad[1][1] + sh.deta[i] * am.grad[2][1];
        }
    }
    const int pe = int(s.vx.cols()), pn = int(s.gx.cols());
    s.et.resize(pe, M);
    const int* edof = es.dofs(t);
    for (int i = 0; i < pe; ++i) s.et.row(i) = m.Et.row(edof[i]);
    s.es.resize(pn, M);
    const int* ndof = ns.dofs(t);
    for (int i = 0; i < pn; ++i) s.es.row(i) = m.Es.row(ndof[i]);
    out.eu.middleRows(row, n).noalias() = s.vx.cast<cd>() * s.et;
    out.ev.middleRows(row, n).noalias() = s.vy.cast<cd>() * s.et;
    s.gu.noalias() = s.gx.cast<cd>() * s.es;
    s.gz.noalias() = s.gy.cast<cd>() * s.es;
    if (s.jb.size() != M)
        s.jb.resize(M);
    for (int k = 0; k < M; ++k) s.jb(k) = J * m.beta[std::size_t(k)];
    const double omega = m.k0 * C0;
    // (µ the triangle's, but a PML's and a bend's vary across it.)
    const bool varies = m.bendRadius > 0 || (t < int(m.layerOf.size()) && m.layerOf[std::size_t(t)] >= 0);
    cd mu3[3];
    modeMu(m, t, am, xi[0], eta[0], mu3);
    for (int p = 0; p < n; ++p) {
        if (varies && p > 0) modeMu(m, t, am, xi[p], eta[p], mu3);
        // ∇×E with ∂s = -jβ: h_u ∝ ∂v Es + jβ Ev, h_v ∝ -jβ Eu - ∂u Es.
        const cd fu = J / (omega * Mu0 * mu3[0]), fv = J / (omega * Mu0 * mu3[1]);
        out.hu.row(row + p) = fu * (s.gz.row(p) + s.jb.cwiseProduct(out.ev.row(row + p)));
        out.hv.row(row + p) = fv * (-s.jb.cwiseProduct(out.eu.row(row + p)) - s.gu.row(p));
    }
}

} // namespace

bool EmeModes::inWindow(int m) const
{
    if (m < 0 || m >= count()) return false;
    const cd n = neff(m);
    const double layered = std::size_t(m) < inLayers.size() ? inLayers[std::size_t(m)] : 0.0;
    return layered < 0.5 && std::abs(n.imag()) <= 0.05 * std::max(std::abs(n.real()), 1e-12);
}

QString EmeModes::label(int m) const
{
    if (m < 0 || m >= count()) return {};
    // (In the window's order of real index: the solver's order, kept.)
    std::vector<char> in(static_cast<std::size_t>(count()), 0);
    for (int i = 0; i < count(); ++i) in[std::size_t(i)] = inWindow(i) ? 1 : 0;
    return labelOf(te, in, m);
}

int EmeModes::find(const QString& text) const
{
    const QString t = text.trimmed().toUpper();
    bool numeric = false;
    const int n = t.toInt(&numeric);
    if (numeric) {
        // (The n-th of the window's modes, by index.)
        int k = 0;
        for (int m = 0; m < count(); ++m)
            if (inWindow(m) && ++k == n) return m;
        return -1;
    }
    for (int m = 0; m < count(); ++m)
        if (label(m) == t) return m;
    return -1;
}

void EmeModes::fieldsAt(int t, double xi, double eta, Eigen::MatrixXcd& out) const
{
    const Field& f = solution->fields().front();
    const EdgeSpace& es = *f.edges;
    const Space& ns = *f.space;
    const AffineMap am = affineMap(ns, t);
    EdgeSpace::Basis b;
    es.basis(t, am, xi, eta, b);
    Shape sh;
    shapeAt(ns.order(), xi, eta, sh);
    const int M = count();
    out.setZero(6, M);
    Eigen::RowVectorXcd eu = Eigen::RowVectorXcd::Zero(M), ev = eu, curl = eu, esv = eu, esu = eu, esz = eu;
    const int* edof = es.dofs(t);
    for (int i = 0; i < b.count; ++i) {
        const auto row = Et.row(edof[i]);
        eu += b.vx[i] * row;
        ev += b.vy[i] * row;
        curl += b.curl[i] * row;
    }
    const int* ndof = ns.dofs(t);
    for (int i = 0; i < sh.count; ++i) {
        const auto row = Es.row(ndof[i]);
        const double gx = sh.dxi[i] * am.grad[1][0] + sh.deta[i] * am.grad[2][0];
        const double gy = sh.dxi[i] * am.grad[1][1] + sh.deta[i] * am.grad[2][1];
        esv += sh.n[i] * row;
        esu += gx * row;
        esz += gy * row;
    }
    const double omega = k0 * C0;
    // µ along u, v, s: in a PML stretched, in a bend times f, f, 1/f.
    cd mu3[3];
    modeMu(*this, t, am, xi, eta, mu3);
    const cd fu = J / (omega * Mu0 * mu3[0]), fv = J / (omega * Mu0 * mu3[1]), fs = J / (omega * Mu0 * mu3[2]);
    out.row(0) = eu;
    out.row(1) = ev;
    out.row(2) = esv;
    for (int m = 0; m < M; ++m) {
        const cd beta = this->beta[std::size_t(m)];
        // ∇×E with ∂s = -jβ: (∂v Es + jβ Ev, -jβ Eu - ∂u Es, ∂u Ev - ∂v Eu).
        out(3, m) = fu * (esz(m) + J * beta * ev(m));
        out(4, m) = fv * (-J * beta * eu(m) - esu(m));
        out(5, m) = fs * curl(m);
    }
}

bool EmeModes::fieldAt(int m, double u, double v, cd E[3], cd H[3]) const
{
    if (!solution || m < 0 || m >= count()) return false;
    Locator loc(solution->mesh());
    int t = -1;
    double xi = 0, eta = 0;
    if (!loc.locate(QPointF(u, v), &t, &xi, &eta)) return false;
    Eigen::MatrixXcd f;
    fieldsAt(t, xi, eta, f);
    for (int c = 0; c < 3; ++c) {
        E[c] = f(c, m);
        H[c] = f(3 + c, m);
    }
    return true;
}

namespace {

/// The degree of a rule exact for two fields of \a order's elements
/// multiplied (E of degree order, H of degree order too).
int ruleDegree(int order)
{
    return std::min(5, 2 * order + 1);
}

/// ∫ (e_m × h_n)·ŝ of one mode set with itself, on its own mesh.
Eigen::MatrixXcd selfOverlap(const EmeModes& a)
{
    const Mesh& mesh = a.solution->mesh();
    const Field& f = a.solution->fields().front();
    const TriangleRule& rule = triangleRule(ruleDegree(f.space->order()));
    const int M = a.count();
    Eigen::MatrixXcd O = Eigen::MatrixXcd::Zero(M, M);
    Eigen::MatrixXcd fields;
    for (int t = 0; t < int(mesh.triangles.size()); ++t) {
        const TriangleMap tm(mesh, t);
        const double jac = 2 * std::abs(tm.area);
        for (const auto& q : rule.points) {
            a.fieldsAt(t, q[0], q[1], fields);
            const double w = q[2] * jac;
            O.noalias() += w * (fields.row(0).transpose() * fields.row(4) - fields.row(1).transpose() * fields.row(3));
        }
    }
    return O;
}

/// The triangles of a mesh on a grid of their boxes: those a box may
/// overlap.
class TriangleGrid
{
public:
    explicit TriangleGrid(const Mesh& mesh) : a_mesh(mesh)
    {
        // (Its nodes' box, by their least and greatest: QRectF's united()
        // passes over a rectangle of no size - each node's -, which left the
        // box a point, every triangle in every cell and the index of a cell
        // past an int's range - UBSan's find.)
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        for (const QPointF& p : mesh.nodes) {
            x0 = std::min(x0, p.x());
            y0 = std::min(y0, p.y());
            x1 = std::max(x1, p.x());
            y1 = std::max(y1, p.y());
        }
        a_box = mesh.nodes.empty() ? QRectF(0, 0, 1, 1) : QRectF(QPointF(x0, y0), QPointF(x1, y1));
        const int n = std::max(1, int(std::sqrt(double(mesh.triangles.size()))));
        a_nx = a_ny = std::max(1, n / 2);
        a_cw = std::max(a_box.width(), 1e-300) / a_nx;
        a_ch = std::max(a_box.height(), 1e-300) / a_ny;
        a_cells.assign(std::size_t(a_nx * a_ny), {});
        for (int t = 0; t < int(mesh.triangles.size()); ++t) {
            int i0, i1, j0, j1;
            range(boxOf(t), &i0, &i1, &j0, &j1);
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) a_cells[std::size_t(j * a_nx + i)].push_back(t);
        }
    }
    QRectF boxOf(int t) const
    {
        const auto& tv = a_mesh.triangles[std::size_t(t)];
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        for (int k = 0; k < 3; ++k) {
            const QPointF p = a_mesh.nodes[std::size_t(tv[std::size_t(k)])];
            x0 = std::min(x0, p.x());
            y0 = std::min(y0, p.y());
            x1 = std::max(x1, p.x());
            y1 = std::max(y1, p.y());
        }
        return QRectF(QPointF(x0, y0), QPointF(x1, y1));
    }
    /// Triangles whose boxes may meet \a box (each once).
    void candidates(const QRectF& box, std::vector<int>& out, std::vector<int>& seen, int stamp) const
    {
        out.clear();
        int i0, i1, j0, j1;
        range(box, &i0, &i1, &j0, &j1);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                for (int t : a_cells[std::size_t(j * a_nx + i)])
                    if (seen[std::size_t(t)] != stamp) {
                        seen[std::size_t(t)] = stamp;
                        out.push_back(t);
                    }
    }

private:
    void range(const QRectF& b, int* i0, int* i1, int* j0, int* j1) const
    {
        // (Clamped as doubles first: a box far outside stays an int's.)
        auto cell = [](double v, int n) { return int(std::clamp(std::floor(v), 0.0, double(n - 1))); };
        *i0 = cell((b.left() - a_box.left()) / a_cw, a_nx);
        *i1 = cell((b.right() - a_box.left()) / a_cw, a_nx);
        *j0 = cell((b.top() - a_box.top()) / a_ch, a_ny);
        *j1 = cell((b.bottom() - a_box.top()) / a_ch, a_ny);
    }
    const Mesh& a_mesh;
    QRectF a_box;
    int a_nx = 1, a_ny = 1;
    double a_cw = 1, a_ch = 1;
    std::vector<std::vector<int>> a_cells;
};

/// A triangle clipped by another's three half-planes: at most six corners
/// (nine kept for rounding's sake) - no heap per clip.
struct ClipPolygon {
    QPointF p[9];
    int n = 0;
};

/// \a in clipped to the half-plane left of the edge a→b, into \a out.
void clipHalf(const ClipPolygon& in, QPointF a, QPointF b, ClipPolygon& out)
{
    out.n = 0;
    const double ex = b.x() - a.x(), ey = b.y() - a.y();
    auto side = [&](QPointF r) { return ex * (r.y() - a.y()) - ey * (r.x() - a.x()); };
    for (int i = 0; i < in.n && out.n < 8; ++i) {
        const QPointF u = in.p[i], v = in.p[(i + 1) % in.n];
        const double su = side(u), sv = side(v);
        if (su >= 0) out.p[out.n++] = u;
        if ((su >= 0) != (sv >= 0)) {
            const double w = su / (su - sv);
            out.p[out.n++] = u + (v - u) * w;
        }
    }
}

/// \a poly clipped to the half-plane left of the edge p→q (counter-clockwise
/// triangles keep their inside on the left).
std::vector<QPointF> clipHalf(const std::vector<QPointF>& poly, QPointF p, QPointF q)
{
    std::vector<QPointF> out;
    const double ex = q.x() - p.x(), ey = q.y() - p.y();
    auto side = [&](QPointF r) { return ex * (r.y() - p.y()) - ey * (r.x() - p.x()); };
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const QPointF a = poly[i], b = poly[(i + 1) % poly.size()];
        const double sa = side(a), sb = side(b);
        if (sa >= 0) out.push_back(a);
        if ((sa >= 0) != (sb >= 0)) {
            const double w = sa / (sa - sb);
            out.push_back(a + (b - a) * w);
        }
    }
    return out;
}

} // namespace

Eigen::MatrixXcd modeOverlap(const EmeModes& a, const EmeModes& b)
{
    Eigen::MatrixXcd ab;
    modeOverlaps(a, b, &ab, nullptr);
    return ab;
}

void modeOverlapsByPoint(const EmeModes& a, const EmeModes& b, Eigen::MatrixXcd* ab, Eigen::MatrixXcd* ba)
{
    const int Ma = a.count(), Mb = b.count();
    Eigen::MatrixXcd& O = *ab;
    O = Eigen::MatrixXcd::Zero(Ma, Mb);
    if (ba) *ba = Eigen::MatrixXcd::Zero(Mb, Ma);
    if (Ma == 0 || Mb == 0) return;
    // ∫ (e_m × h_n)·ŝ = e_u h_v - e_v h_u, both ways, at each point.
    auto add = [&](const Eigen::MatrixXcd& fa, const Eigen::MatrixXcd& fb, double w) {
        O.noalias() += w * (fa.row(0).transpose() * fb.row(4) - fa.row(1).transpose() * fb.row(3));
        if (ba) ba->noalias() += w * (fb.row(0).transpose() * fa.row(4) - fb.row(1).transpose() * fa.row(3));
    };
    if (a.solution == b.solution) {
        // (The same cut solved once: its own mesh, exactly.)
        const Mesh& mesh = a.solution->mesh();
        const TriangleRule& rule = triangleRule(ruleDegree(a.solution->fields().front().space->order()));
        Eigen::MatrixXcd fa, fb;
        for (int t = 0; t < int(mesh.triangles.size()); ++t) {
            const TriangleMap tm(mesh, t);
            const double jac = 2 * std::abs(tm.area);
            for (const auto& q : rule.points) {
                a.fieldsAt(t, q[0], q[1], fa);
                b.fieldsAt(t, q[0], q[1], fb);
                add(fa, fb, q[2] * jac);
            }
        }
        return;
    }
    const Mesh& ma = a.solution->mesh();
    const Mesh& mb = b.solution->mesh();
    const int order = std::max(a.solution->fields().front().space->order(), b.solution->fields().front().space->order());
    const TriangleRule& rule = triangleRule(ruleDegree(order));
    const TriangleGrid grid(mb);
    std::vector<int> cand, seen(mb.triangles.size(), -1);
    Eigen::MatrixXcd fa, fb;
    // (Coordinates in the geometry's unit for the clip; areas and points in
    // metres for the integral.)
    const double sa = ma.unitScale;
    for (int ta = 0; ta < int(ma.triangles.size()); ++ta) {
        const auto& av = ma.triangles[std::size_t(ta)];
        std::vector<QPointF> A{ma.nodes[std::size_t(av[0])], ma.nodes[std::size_t(av[1])], ma.nodes[std::size_t(av[2])]};
        // (Counter-clockwise.)
        const double sgn = (A[1].x() - A[0].x()) * (A[2].y() - A[0].y()) - (A[1].y() - A[0].y()) * (A[2].x() - A[0].x());
        if (sgn < 0) std::swap(A[1], A[2]);
        QRectF box(QPointF(std::min({A[0].x(), A[1].x(), A[2].x()}), std::min({A[0].y(), A[1].y(), A[2].y()})),
                   QPointF(std::max({A[0].x(), A[1].x(), A[2].x()}), std::max({A[0].y(), A[1].y(), A[2].y()})));
        grid.candidates(box, cand, seen, ta);
        const TriangleMap mapA(ma, ta);
        const double tiny = 1e-14 * std::abs(mapA.area) / (sa * sa);
        for (int tb : cand) {
            const auto& bv = mb.triangles[std::size_t(tb)];
            std::vector<QPointF> poly{mb.nodes[std::size_t(bv[0])], mb.nodes[std::size_t(bv[1])], mb.nodes[std::size_t(bv[2])]};
            for (int k = 0; k < 3 && poly.size() >= 3; ++k) poly = clipHalf(poly, A[std::size_t(k)], A[std::size_t((k + 1) % 3)]);
            if (poly.size() < 3) continue;
            const TriangleMap mapB(mb, tb);
            for (std::size_t k = 1; k + 1 < poly.size(); ++k) {
                const QPointF p0 = poly[0], p1 = poly[k], p2 = poly[k + 1];
                const double area = 0.5 * std::abs((p1.x() - p0.x()) * (p2.y() - p0.y()) - (p1.y() - p0.y()) * (p2.x() - p0.x()));
                if (area <= tiny) continue;
                const double jac = 2 * area * sa * sa;
                for (const auto& q : rule.points) {
                    const QPointF p = (p0 + (p1 - p0) * q[0] + (p2 - p0) * q[1]) * sa;
                    double xa, ea, xb, eb;
                    mapA.reference(p.x(), p.y(), &xa, &ea);
                    mapB.reference(p.x(), p.y(), &xb, &eb);
                    a.fieldsAt(ta, xa, ea, fa);
                    b.fieldsAt(tb, xb, eb, fb);
                    add(fa, fb, q[2] * jac);
                }
            }
        }
    }
}

namespace {

/// Triangles of a in a chunk of an overlap: fixed, so that a pair's sum - its
/// chunks added in their order - is the same to the last bit however many
/// threads share its chunks.
constexpr int OverlapChunk = 512;
/// Points gathered before their products: the products are GEMMs, not a
/// rank-one update a point.
constexpr int OverlapBatch = 512;

/// One thread's buffers for overlapRange().
struct OverlapScratch {
    Transverse A, B;
    TransverseScratch sa, sb;
    std::vector<double> xa, ea, xb, eb, w;
    std::vector<int> tb;                 // each point's triangle of b
    std::vector<std::pair<int, int>> segA;   // (triangle of a, first point), in order
    std::vector<int> cand, seen;
    RowMatrix wa;
};

/// The points gathered, their fields and products: O_ab += Σ w e_u,a h_v,b -
/// e_v,a h_u,b, O_ba += Σ w e_u,b h_v,a - e_v,b h_u,a (no conjugate).
void overlapFlush(const EmeModes& a, const EmeModes& b, OverlapScratch& s, Eigen::MatrixXcd& ab, Eigen::MatrixXcd* ba)
{
    const int P = int(s.w.size());
    if (P == 0) return;
    s.A.reserve(P, a.count());
    s.B.reserve(P, b.count());
    for (std::size_t k = 0; k < s.segA.size(); ++k) {
        const int from = s.segA[k].second, to = k + 1 < s.segA.size() ? s.segA[k + 1].second : P;
        transverseAt(a, s.segA[k].first, s.xa.data() + from, s.ea.data() + from, to - from, s.A, from, s.sa);
    }
    for (int from = 0; from < P;) {
        int to = from + 1;
        while (to < P && s.tb[std::size_t(to)] == s.tb[std::size_t(from)]) ++to;
        transverseAt(b, s.tb[std::size_t(from)], s.xb.data() + from, s.eb.data() + from, to - from, s.B, from, s.sb);
        from = to;
    }
    const Eigen::Map<const Eigen::VectorXd> w(s.w.data(), P);
    s.wa = w.asDiagonal() * s.A.eu.topRows(P);
    ab.noalias() += s.wa.transpose() * s.B.hv.topRows(P);
    s.wa = w.asDiagonal() * s.A.ev.topRows(P);
    ab.noalias() -= s.wa.transpose() * s.B.hu.topRows(P);
    if (ba) {
        s.wa = w.asDiagonal() * s.A.hv.topRows(P);
        ba->noalias() += s.B.eu.topRows(P).transpose() * s.wa;
        s.wa = w.asDiagonal() * s.A.hu.topRows(P);
        ba->noalias() -= s.B.ev.topRows(P).transpose() * s.wa;
    }
    s.xa.clear();
    s.ea.clear();
    s.xb.clear();
    s.eb.clear();
    s.w.clear();
    s.tb.clear();
    s.segA.clear();
}

/// The overlaps over a's triangles [t0, t1): on a's own mesh where the two
/// are one cut, else on the common refinement - each triangle of a clipped by
/// those of b it meets (\a grid: b's), a degree-5 rule on each piece.
void overlapRange(const EmeModes& a, const EmeModes& b, const TriangleGrid* grid, int t0, int t1, OverlapScratch& s, Eigen::MatrixXcd& ab,
                  Eigen::MatrixXcd* ba)
{
    const Mesh& ma = a.solution->mesh();
    const int order = std::max(a.solution->fields().front().space->order(), b.solution->fields().front().space->order());
    const TriangleRule& rule = triangleRule(ruleDegree(order));
    if (a.solution == b.solution) {
        for (int t = t0; t < t1; ++t) {
            const TriangleMap tm(ma, t);
            const double jac = 2 * std::abs(tm.area);
            s.segA.push_back({t, int(s.w.size())});
            for (const auto& q : rule.points) {
                s.xa.push_back(q[0]);
                s.ea.push_back(q[1]);
                s.xb.push_back(q[0]);
                s.eb.push_back(q[1]);
                s.tb.push_back(t);
                s.w.push_back(q[2] * jac);
            }
            if (int(s.w.size()) >= OverlapBatch) overlapFlush(a, b, s, ab, ba);
        }
        overlapFlush(a, b, s, ab, ba);
        return;
    }
    const Mesh& mb = b.solution->mesh();
    s.seen.assign(mb.triangles.size(), -1);
    // (Coordinates in the geometry's unit for the clip; areas and points in
    // metres for the integral.)
    const double sa = ma.unitScale;
    for (int ta = t0; ta < t1; ++ta) {
        const auto& av = ma.triangles[std::size_t(ta)];
        std::vector<QPointF> A{ma.nodes[std::size_t(av[0])], ma.nodes[std::size_t(av[1])], ma.nodes[std::size_t(av[2])]};
        // (Counter-clockwise.)
        const double sgn = (A[1].x() - A[0].x()) * (A[2].y() - A[0].y()) - (A[1].y() - A[0].y()) * (A[2].x() - A[0].x());
        if (sgn < 0) std::swap(A[1], A[2]);
        QRectF box(QPointF(std::min({A[0].x(), A[1].x(), A[2].x()}), std::min({A[0].y(), A[1].y(), A[2].y()})),
                   QPointF(std::max({A[0].x(), A[1].x(), A[2].x()}), std::max({A[0].y(), A[1].y(), A[2].y()})));
        grid->candidates(box, s.cand, s.seen, ta);
        const TriangleMap mapA(ma, ta);
        const double tiny = 1e-14 * std::abs(mapA.area) / (sa * sa);
        const int first = int(s.w.size());
        for (int tb : s.cand) {
            const auto& bv = mb.triangles[std::size_t(tb)];
            ClipPolygon poly, other;
            poly.n = 3;
            for (int k = 0; k < 3; ++k) poly.p[k] = mb.nodes[std::size_t(bv[std::size_t(k)])];
            for (int k = 0; k < 3 && poly.n >= 3; ++k) {
                clipHalf(poly, A[std::size_t(k)], A[std::size_t((k + 1) % 3)], other);
                poly = other;
            }
            if (poly.n < 3) continue;
            const TriangleMap mapB(mb, tb);
            for (int k = 1; k + 1 < poly.n; ++k) {
                const QPointF p0 = poly.p[0], p1 = poly.p[k], p2 = poly.p[k + 1];
                const double area = 0.5 * std::abs((p1.x() - p0.x()) * (p2.y() - p0.y()) - (p1.y() - p0.y()) * (p2.x() - p0.x()));
                if (area <= tiny) continue;
                const double jac = 2 * area * sa * sa;
                for (const auto& q : rule.points) {
                    const QPointF p = (p0 + (p1 - p0) * q[0] + (p2 - p0) * q[1]) * sa;
                    double xa, ea, xb, eb;
                    mapA.reference(p.x(), p.y(), &xa, &ea);
                    mapB.reference(p.x(), p.y(), &xb, &eb);
                    s.xa.push_back(xa);
                    s.ea.push_back(ea);
                    s.xb.push_back(xb);
                    s.eb.push_back(eb);
                    s.tb.push_back(tb);
                    s.w.push_back(q[2] * jac);
                }
            }
        }
        if (int(s.w.size()) > first) s.segA.push_back({ta, first});
        if (int(s.w.size()) >= OverlapBatch) overlapFlush(a, b, s, ab, ba);
    }
    overlapFlush(a, b, s, ab, ba);
}

} // namespace

bool modeOverlapsMany(const std::vector<std::pair<const EmeModes*, const EmeModes*>>& pairs, int threads, const std::function<bool()>& cancelled,
                      const std::function<void(int, int)>& progress, std::vector<Eigen::MatrixXcd>* ab, std::vector<Eigen::MatrixXcd>* ba)
{
    const int n = int(pairs.size());
    ab->assign(std::size_t(n), Eigen::MatrixXcd());
    if (ba) ba->assign(std::size_t(n), Eigen::MatrixXcd());
    // Each pair's chunks of its a's triangles, a job each; a chunk done adds
    // itself and those after it that are done, in their order.
    struct Pair {
        std::unique_ptr<TriangleGrid> grid;
        int chunks = 0, next = 0;
        std::vector<std::unique_ptr<std::pair<Eigen::MatrixXcd, Eigen::MatrixXcd>>> parts;
        std::mutex lock;
    };
    std::vector<Pair> state(static_cast<std::size_t>(n));
    std::vector<std::pair<int, int>> jobs;   // (pair, chunk)
    for (int i = 0; i < n; ++i) {
        const EmeModes& a = *pairs[std::size_t(i)].first;
        const EmeModes& b = *pairs[std::size_t(i)].second;
        (*ab)[std::size_t(i)] = Eigen::MatrixXcd::Zero(a.count(), b.count());
        if (ba) (*ba)[std::size_t(i)] = Eigen::MatrixXcd::Zero(b.count(), a.count());
        if (a.count() == 0 || b.count() == 0) continue;
        Pair& p = state[std::size_t(i)];
        if (a.solution != b.solution) p.grid = std::make_unique<TriangleGrid>(b.solution->mesh());
        const int triangles = int(a.solution->mesh().triangles.size());
        p.chunks = (triangles + OverlapChunk - 1) / OverlapChunk;
        p.parts.resize(std::size_t(p.chunks));
        for (int c = 0; c < p.chunks; ++c) jobs.push_back({i, c});
    }
    std::atomic<int> next{0}, done{0};
    std::atomic<bool> stop{false};
    std::mutex telling;
    const int total = int(jobs.size());
    auto work = [&] {
        OverlapScratch s;
        for (int j = next++; j < total && !stop; j = next++) {
            if (cancelled && cancelled()) {
                stop = true;
                return;
            }
            const auto [i, c] = jobs[std::size_t(j)];
            const EmeModes& a = *pairs[std::size_t(i)].first;
            const EmeModes& b = *pairs[std::size_t(i)].second;
            Pair& p = state[std::size_t(i)];
            auto part = std::make_unique<std::pair<Eigen::MatrixXcd, Eigen::MatrixXcd>>(Eigen::MatrixXcd::Zero(a.count(), b.count()),
                                                                                        ba ? Eigen::MatrixXcd::Zero(b.count(), a.count()) : Eigen::MatrixXcd());
            const int t0 = c * OverlapChunk, t1 = std::min(int(a.solution->mesh().triangles.size()), t0 + OverlapChunk);
            overlapRange(a, b, p.grid.get(), t0, t1, s, part->first, ba ? &part->second : nullptr);
            {
                std::lock_guard<std::mutex> lock(p.lock);
                p.parts[std::size_t(c)] = std::move(part);
                while (p.next < p.chunks && p.parts[std::size_t(p.next)]) {
                    (*ab)[std::size_t(i)] += p.parts[std::size_t(p.next)]->first;
                    if (ba) (*ba)[std::size_t(i)] += p.parts[std::size_t(p.next)]->second;
                    p.parts[std::size_t(p.next)].reset();
                    ++p.next;
                }
            }
            const int finished = ++done;
            if (progress) {
                std::lock_guard<std::mutex> lock(telling);
                progress(finished, total);
            }
        }
    };
    const int count = std::clamp(threads > 0 ? threads : int(std::thread::hardware_concurrency()), 1, std::max(1, total));
    std::vector<std::thread> pool;
    for (int t = 1; t < count; ++t) pool.emplace_back(work);
    work();
    for (std::thread& t : pool) t.join();
    return !stop;
}

void modeOverlaps(const EmeModes& a, const EmeModes& b, Eigen::MatrixXcd* ab, Eigen::MatrixXcd* ba)
{
    std::vector<Eigen::MatrixXcd> abs, bas;
    modeOverlapsMany({{&a, &b}}, 1, {}, {}, &abs, ba ? &bas : nullptr);
    *ab = std::move(abs.front());
    if (ba) *ba = std::move(bas.front());
}

namespace {

/// Modes \a kept of \a m alone, in their order.
void keepModes(EmeModes& m, const std::vector<int>& kept)
{
    if (int(kept.size()) == m.count()) return;
    EmeModes k = m;
    k.beta.clear();
    k.te.clear();
    k.inLayers.clear();
    k.Et.resize(m.Et.rows(), int(kept.size()));
    k.Es.resize(m.Es.rows(), int(kept.size()));
    for (int i = 0; i < int(kept.size()); ++i) {
        const int j = kept[std::size_t(i)];
        k.beta.push_back(m.beta[std::size_t(j)]);
        k.te.push_back(m.te[std::size_t(j)]);
        if (std::size_t(j) < m.inLayers.size()) k.inLayers.push_back(m.inLayers[std::size_t(j)]);
        k.Et.col(i) = m.Et.col(j);
        k.Es.col(i) = m.Es.col(j);
    }
    m = std::move(k);
}

/// The modes normalized to ⟨e_m, h_m⟩ = 1 - a degenerate cluster's made
/// bi-orthogonal first (Gram-Schmidt in the bilinear form) - and their
/// signs fixed: the main transverse field's weighted integral positive. A
/// mode whose ⟨e, h⟩ is next to nothing beside ∫|e||h| - all but
/// orthogonal to itself, as a lossy cut's can be - or not finite is left
/// out: normalized, it was huge, and an S-matrix NaN.
/// The signs of \a m's modes from the integrals of their main transverse
/// field over the window: ∫ e_main, then ∫ (u - uc) e_main, then ∫ (v - vc)
/// e_main, the first not nought positive.
void signModes(EmeModes& m, const Eigen::MatrixXcd& I, const Eigen::VectorXd& absInt)
{
    const double width = (m.section.u1 - m.section.u0) * m.unitScale;
    for (int k = 0; k < m.count(); ++k) {
        double s = 0;
        if (std::abs(I(0, k)) > 1e-3 * absInt(k)) s = I(0, k).real();
        else if (std::abs(I(1, k)) > 1e-3 * absInt(k) * width) s = I(1, k).real();
        else s = I(2, k).real();
        if (s < 0) {
            m.Et.col(k) *= -1.0;
            m.Es.col(k) *= -1.0;
        }
    }
}

void normalize(EmeModes& m)
{
    if (m.count() == 0) return;
    {
        const Eigen::MatrixXcd G0 = selfOverlap(m);
        // ∫ (|e_u||h_v| + |e_v||h_u|), mode by mode.
        const Mesh& mesh = m.solution->mesh();
        const TriangleRule& rule = triangleRule(2);
        Eigen::VectorXd scale = Eigen::VectorXd::Zero(m.count());
        Eigen::MatrixXcd f;
        for (int t = 0; t < int(mesh.triangles.size()); ++t) {
            const TriangleMap tm(mesh, t);
            const double jac = 2 * std::abs(tm.area);
            for (const auto& q : rule.points) {
                m.fieldsAt(t, q[0], q[1], f);
                for (int k = 0; k < m.count(); ++k)
                    scale(k) += q[2] * jac * (std::abs(f(0, k)) * std::abs(f(4, k)) + std::abs(f(1, k)) * std::abs(f(3, k)));
            }
        }
        std::vector<int> kept;
        for (int k = 0; k < m.count(); ++k) {
            const cd g = G0(k, k);
            if (std::isfinite(g.real()) && std::isfinite(g.imag()) && std::isfinite(scale(k)) && std::abs(g) > 1e-6 * scale(k) &&
                std::isfinite(m.beta[std::size_t(k)].real()) && std::isfinite(m.beta[std::size_t(k)].imag()))
                kept.push_back(k);
        }
        keepModes(m, kept);
    }
    const int M = m.count();
    if (M == 0) return;
    Eigen::MatrixXcd G = selfOverlap(m);
    // Degenerate clusters: β alike to 1e-8.
    std::vector<int> cluster(std::size_t(M), -1);
    for (int i = 0; i < M; ++i) {
        if (cluster[std::size_t(i)] >= 0) continue;
        cluster[std::size_t(i)] = i;
        for (int j = i + 1; j < M; ++j)
            if (std::abs(m.beta[std::size_t(j)] - m.beta[std::size_t(i)]) <= 1e-8 * std::abs(m.beta[std::size_t(i)])) cluster[std::size_t(j)] = i;
    }
    bool mixed = false;
    for (int i = 0; i < M; ++i) {
        // (Each later member of a cluster made orthogonal to the earlier ones.)
        for (int j = 0; j < i; ++j) {
            if (cluster[std::size_t(j)] != cluster[std::size_t(i)]) continue;
            const cd gjj = G(j, j), gji = G(j, i);
            if (std::abs(gjj) < 1e-300) continue;
            const cd c = gji / gjj;
            m.Et.col(i) -= c * m.Et.col(j);
            m.Es.col(i) -= c * m.Es.col(j);
            mixed = true;
            // G's row and column i follow.
            G.col(i) -= c * G.col(j);
            G.row(i) -= c * G.row(j);
        }
    }
    if (mixed) G = selfOverlap(m);
    for (int i = 0; i < M; ++i) {
        cd g = G(i, i);
        if (std::abs(g) < 1e-300) g = 1;
        const cd s = 1.0 / std::sqrt(g);
        m.Et.col(i) *= s;
        m.Es.col(i) *= s;
    }
    // Signs: ∫ e_main, then ∫ (u - uc) e_main, then ∫ (v - vc) e_main.
    const Mesh& mesh = m.solution->mesh();
    const TriangleRule& rule = triangleRule(2);
    const double uc = (m.section.u0 + m.section.u1) / 2 * mesh.unitScale, vc = (m.section.z0 + m.section.z1) / 2 * mesh.unitScale;
    Eigen::MatrixXcd I = Eigen::MatrixXcd::Zero(3, M);
    Eigen::VectorXd absInt = Eigen::VectorXd::Zero(M), all = Eigen::VectorXd::Zero(M), layered = Eigen::VectorXd::Zero(M);
    Eigen::MatrixXcd f;
    for (int t = 0; t < int(mesh.triangles.size()); ++t) {
        const TriangleMap tm(mesh, t);
        const double jac = 2 * std::abs(tm.area);
        const bool inLayer = (t < int(m.layerOf.size()) && m.layerOf[std::size_t(t)] >= 0) || (t < int(m.inAbsorber.size()) && m.inAbsorber[std::size_t(t)]);
        for (const auto& q : rule.points) {
            m.fieldsAt(t, q[0], q[1], f);
            const double x = tm.x0 + tm.a * q[0] + tm.b * q[1], y = tm.y0 + tm.c * q[0] + tm.d * q[1];
            const double w = q[2] * jac;
            for (int k = 0; k < M; ++k) {
                const cd e = m.te[std::size_t(k)] >= 0.5 ? f(0, k) : f(1, k);
                const double e2 = std::norm(f(0, k)) + std::norm(f(1, k));
                all(k) += w * e2;
                if (inLayer) layered(k) += w * e2;
                // (The sign's integrals over the window alone.)
                if (inLayer) continue;
                I(0, k) += w * e;
                I(1, k) += w * (x - uc) * e;
                I(2, k) += w * (y - vc) * e;
                absInt(k) += w * std::abs(e);
            }
        }
    }
    m.inLayers.assign(std::size_t(M), 0.0);
    for (int k = 0; k < M; ++k) m.inLayers[std::size_t(k)] = all(k) > 0 ? layered(k) / all(k) : 0.0;
    signModes(m, I, absInt);
}

} // namespace

void orientModes(EmeModes& m, bool mirrored)
{
    const int M = m.count();
    if (M == 0 || !m.solution) return;
    // The sign rule's integrals in the mirrored frame: u and E_u turned over.
    const double flip = mirrored ? -1.0 : 1.0;
    const Mesh& mesh = m.solution->mesh();
    const TriangleRule& rule = triangleRule(2);
    const double uc = (m.section.u0 + m.section.u1) / 2 * mesh.unitScale, vc = (m.section.z0 + m.section.z1) / 2 * mesh.unitScale;
    Eigen::MatrixXcd I = Eigen::MatrixXcd::Zero(3, M);
    Eigen::VectorXd absInt = Eigen::VectorXd::Zero(M);
    Eigen::MatrixXcd f;
    for (int t = 0; t < int(mesh.triangles.size()); ++t) {
        const bool inLayer = (t < int(m.layerOf.size()) && m.layerOf[std::size_t(t)] >= 0) || (t < int(m.inAbsorber.size()) && m.inAbsorber[std::size_t(t)]);
        if (inLayer) continue;
        const TriangleMap tm(mesh, t);
        const double jac = 2 * std::abs(tm.area);
        for (const auto& q : rule.points) {
            m.fieldsAt(t, q[0], q[1], f);
            const double x = tm.x0 + tm.a * q[0] + tm.b * q[1], y = tm.y0 + tm.c * q[0] + tm.d * q[1];
            const double w = q[2] * jac;
            for (int k = 0; k < M; ++k) {
                const cd e = m.te[std::size_t(k)] >= 0.5 ? flip * f(0, k) : f(1, k);
                I(0, k) += w * e;
                I(1, k) += w * flip * (x - uc) * e;
                I(2, k) += w * (y - vc) * e;
                absInt(k) += w * std::abs(e);
            }
        }
    }
    signModes(m, I, absInt);
}

namespace {

/*!
 * Cuts solved, kept: a cut's modes depend on nothing but its 2D model (its
 * rectangles, their materials, its mesh's sizes, its walls, the modes asked,
 * the frequency) and the parameters that model reads - so a length swept
 * (an MMI's, a coupler's) solves each cut once, not once a point. Keyed by
 * that model's JSON and those parameters' values; the most recently used
 * first, some 2 GB at most (each cut's coefficients twice: ours and the
 * solution's snapshots).
 */
struct CachedCut {
    QByteArray key;
    EmeModes modes;
    std::size_t bytes = 0;
};
std::mutex cutCacheMutex;
std::list<CachedCut>& cutCache()
{
    static std::list<CachedCut> cache;
    return cache;
}
std::size_t cutCacheBytes = 0;
constexpr std::size_t CutCacheLimit = std::size_t(2) << 30;

bool cachedCut(const QByteArray& key, EmeModes* out)
{
    std::lock_guard<std::mutex> lock(cutCacheMutex);
    auto& cache = cutCache();
    for (auto it = cache.begin(); it != cache.end(); ++it)
        if (it->key == key) {
            cache.splice(cache.begin(), cache, it);
            *out = cache.front().modes;
            return true;
        }
    return false;
}

void keepCut(const QByteArray& key, const EmeModes& modes)
{
    const std::size_t bytes = std::size_t(2 * 16) * std::size_t(modes.count()) * std::size_t(modes.Et.rows() + modes.Es.rows());
    if (bytes > CutCacheLimit / 4) return;
    std::lock_guard<std::mutex> lock(cutCacheMutex);
    auto& cache = cutCache();
    for (const CachedCut& c : cache)
        if (c.key == key) return;
    cache.push_front({key, modes, bytes});
    cutCacheBytes += bytes;
    while (cutCacheBytes > CutCacheLimit && cache.size() > 1) {
        cutCacheBytes -= cache.back().bytes;
        cache.pop_back();
    }
}

/// Each 3D domain's material node, as the model gives it: the last enabled
/// material whose selection has it (null: none).
std::vector<const Node*> domainMaterials(const Model& model, const Topology3& topology)
{
    std::vector<const Node*> ofDomain(topology.domains.size(), nullptr);
    for (const Node& m : model.materials().children) {
        if (!m.enabled || m.type != QLatin1String("material")) continue;
        QString why;
        for (int d : resolveSelection3(topology, model, Level::Domain, m.selection(), &why))
            if (d >= 0 && d < int(ofDomain.size())) ofDomain[std::size_t(d)] = &m;
    }
    return ofDomain;
}

/// The index of a material's node at \a lambda (m), its real part: n, or
/// √εr; 1 where it gives neither.
double indexOf(const Node& material, double lambda, const Scope& scope)
{
    MaterialSweep sw;
    sw.variable = QStringLiteral("lambda");
    sw.from = sw.to = lambda;
    sw.points = 1;
    sw.lambda = lambda;
    const MaterialCurves c = materialCurves(material, {QStringLiteral("n")}, sw, scope);
    for (const MaterialCurve& k : c.curves)
        if (k.key == QLatin1String("n") && !k.values.empty() && std::isfinite(k.values.front()) && k.values.front() > 0) return k.values.front();
    const MaterialCurves e = materialCurves(material, {QStringLiteral("epsilonr")}, sw, scope);
    for (const MaterialCurve& k : e.curves)
        if (k.key == QLatin1String("epsilonr") && !k.values.empty() && std::isfinite(k.values.front()) && k.values.front() > 0)
            return std::sqrt(k.values.front());
    return 1;
}

} // namespace

EmeModes solveSectionModes(const Model& model, std::shared_ptr<ParameterScope> parameters, const Topology3& topology,
                           const CrossSection& section, double frequency, const EmeSettings& settings, QString* error)
{
    EmeModes out;
    out.section = section;
    out.frequency = frequency;
    out.k0 = 2 * M_PI * frequency / C0;
    out.unitScale = topology.unitScale;
    if (section.rects.empty()) {
        *error = tr("the cut has no rectangle");
        return out;
    }
    if (!parameters) parameters = evaluateParameters(model);
    const double lambda = C0 / frequency;
    // The materials of each 3D domain: the last that has it, as the model's
    // (domainMaterials()); each one's domains for the cut's model below.
    std::vector<const Node*> materials;
    std::vector<QVector<int>> picks;
    for (const Node& m : model.materials().children) {
        if (!m.enabled || m.type != QLatin1String("material")) continue;
        QString why;
        picks.push_back(resolveSelection3(topology, model, Level::Domain, m.selection(), &why));
        materials.push_back(&m);
    }
    const std::vector<const Node*> ofDomain = domainMaterials(model, topology);
    // The window's rectangles, and its PMLs': each edge rectangle carried
    // out past the window by the layer's thickness, of its own material -
    // a corner's both ways where both walls are layers.
    // The window's rectangles, and its walls' layers - a PML's, or an
    // absorbing wall's in steps of rising extinction: each edge rectangle
    // carried out past the window, of its own material - a corner's both
    // ways where both walls are layers.
    std::vector<CrossSection::Rect> rects = section.rects;
    std::vector<char> isLayer(rects.size(), 0);
    std::vector<double> absorbKappa(rects.size(), 0.0);
    {
        const bool pmlSides = settings.sides == QLatin1String("pml"), pmlFloors = settings.floors == QLatin1String("pml");
        const bool absSides = settings.sides == QLatin1String("absorbing"), absFloors = settings.floors == QLatin1String("absorbing");
        if ((pmlSides || pmlFloors) && (absSides || absFloors)) {
            *error = tr("a cut's walls are PMLs or absorbing, not both");
            return out;
        }
        const bool sides = pmlSides || absSides, floors = pmlFloors || absFloors, absorbing = absSides || absFloors;
        const double d = absorbing ? settings.absorberThickness : settings.pmlThickness;
        const int N = absorbing ? std::max(1, settings.absorberLayers) : 1;
        const double tol = 1e-9 * std::max({1.0, section.u1 - section.u0, section.z1 - section.z0});
        auto kappaOf = [&](int i) { return settings.absorberKappa * (N == 1 ? 1.0 : std::pow((i + 0.5) / N, 2)); };
        auto add = [&](double u0, double u1, double z0, double z1, int domain, int step) {
            rects.push_back({u0, u1, z0, z1, domain});
            isLayer.push_back(absorbing ? 0 : 1);
            absorbKappa.push_back(absorbing ? kappaOf(step) : 0.0);
        };
        if ((sides || floors) && !(d > 0)) {
            *error = tr("a wall's layer must be more than nought thick");
            return out;
        }
        if (absorbing && !(settings.absorberKappa > 0)) {
            *error = tr("an absorbing wall's extinction must be more than nought");
            return out;
        }
        for (const CrossSection::Rect& r : section.rects) {
            const bool left = std::abs(r.u0 - section.u0) <= tol, right = std::abs(r.u1 - section.u1) <= tol;
            const bool bottom = std::abs(r.z0 - section.z0) <= tol, top = std::abs(r.z1 - section.z1) <= tol;
            for (int i = 0; i < N; ++i) {
                const double a = d * i / N, b = d * (i + 1) / N;
                if (sides && left) add(section.u0 - b, section.u0 - a, r.z0, r.z1, r.domain, i);
                if (sides && right) add(section.u1 + a, section.u1 + b, r.z0, r.z1, r.domain, i);
                if (floors && bottom) add(r.u0, r.u1, section.z0 - b, section.z0 - a, r.domain, i);
                if (floors && top) add(r.u0, r.u1, section.z1 + a, section.z1 + b, r.domain, i);
            }
            if (sides && floors)
                for (int i = 0; i < N; ++i)
                    for (int j = 0; j < N; ++j) {
                        const double ua = d * i / N, ub = d * (i + 1) / N, za = d * j / N, zb = d * (j + 1) / N;
                        const int step = std::max(i, j);
                        if (left && bottom) add(section.u0 - ub, section.u0 - ua, section.z0 - zb, section.z0 - za, r.domain, step);
                        if (left && top) add(section.u0 - ub, section.u0 - ua, section.z1 + za, section.z1 + zb, r.domain, step);
                        if (right && bottom) add(section.u1 + ua, section.u1 + ub, section.z0 - zb, section.z0 - za, r.domain, step);
                        if (right && top) add(section.u1 + ua, section.u1 + ub, section.z1 + za, section.z1 + zb, r.domain, step);
                    }
        }
    }
    // The rectangles of the 3D model's PML domains: PMLs.
    for (std::size_t i = 0; i < section.rects.size(); ++i)
        if (std::find(settings.pmlDomains.begin(), settings.pmlDomains.end(), section.rects[i].domain) != settings.pmlDomains.end()) isLayer[i] = 1;
    // The rectangles, each its element size: λ0/(resolution n), and so many
    // across it at least.
    QJsonArray geometry, sizes;
    QHash<QString, double> indexCache;
    std::vector<double> rectIndex(rects.size(), 1.0);
    double hAll = 0;
    for (std::size_t i = 0; i < rects.size(); ++i) {
        const CrossSection::Rect& r = rects[i];
        const QString name = QStringLiteral("r%1").arg(i + 1);
        geometry.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("rectangle")},
                                    {QStringLiteral("name"), name},
                                    {QStringLiteral("position"), QJsonArray{number(r.u0), number(r.z0)}},
                                    {QStringLiteral("size"), QJsonArray{number(r.u1 - r.u0), number(r.z1 - r.z0)}}});
        const Node* mat = r.domain >= 0 && r.domain < int(ofDomain.size()) ? ofDomain[std::size_t(r.domain)] : nullptr;
        if (!mat) {
            *error = tr("domain %1 has no material: give it one (its refractive index)").arg(r.domain + 1);
            return out;
        }
        double n = 1;
        if (indexCache.contains(mat->tag)) n = indexCache.value(mat->tag);
        else indexCache.insert(mat->tag, n = indexOf(*mat, lambda, parameters->scope));
        rectIndex[i] = n;
        double h = lambda / topology.unitScale / (settings.resolution * std::max(n, 1.0));
        h = std::min(h, std::min(r.u1 - r.u0, r.z1 - r.z0) / std::max(1, settings.across));
        hAll = std::max(hAll, h);
        if (!isLayer[i])
            out.propagating += (r.u1 - r.u0) * (r.z1 - r.z0) * topology.unitScale * topology.unitScale * out.k0 * out.k0 * n * n / (2 * M_PI);
        sizes.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("size")},
                                 {QStringLiteral("level"), QStringLiteral("domain")},
                                 {QStringLiteral("selection"), QJsonObject{{QStringLiteral("objects"), QJsonArray{name}}}},
                                 {QStringLiteral("hmax"), number(h)}});
    }
    // The window's walls: metal by default (and behind a PML); magnetic
    // where asked.
    QJsonArray features;
    QJsonArray all, layerNames;
    for (std::size_t i = 0; i < rects.size(); ++i) {
        all.append(QStringLiteral("r%1").arg(i + 1));
        if (isLayer[i]) layerNames.append(QStringLiteral("r%1").arg(i + 1));
    }
    // ("of" every rectangle with a side: the window's outer boundaries on it.)
    auto wall = [&](const QString& side) {
        features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("pmc")},
                                    {QStringLiteral("selection"), QJsonObject{{QStringLiteral("of"), all}, {QStringLiteral("side"), side}}}});
    };
    if (settings.sides == QLatin1String("pmc")) {
        wall(QStringLiteral("left"));
        wall(QStringLiteral("right"));
    }
    if (settings.floors == QLatin1String("pmc")) {
        wall(QStringLiteral("bottom"));
        wall(QStringLiteral("top"));
    }
    if (!layerNames.isEmpty())
        features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("pml")},
                                    {QStringLiteral("selection"), QJsonObject{{QStringLiteral("objects"), layerNames}}},
                                    {QStringLiteral("reflection"), number(settings.pmlReflection)}});
    const QString unit = model.component().text(QStringLiteral("unit"));
    QJsonObject physics{{QStringLiteral("type"), QStringLiteral("modes")},
                        {QStringLiteral("tag"), QStringLiteral("emw")},
                        {QStringLiteral("order"), QString::number(settings.order)},
                        {QStringLiteral("features"), features}};
    // A bend's cut: the mode solver's bend, its axis where the cut says.
    if (section.bendRadius > 0) {
        physics.insert(QStringLiteral("bendradius"), number(section.bendRadius));
        physics.insert(QStringLiteral("bendcenter"), number(section.bendCenter));
    }
    QJsonObject component{{QStringLiteral("unit"), unit.isEmpty() ? QStringLiteral("um") : unit},
                          {QStringLiteral("geometry"), geometry},
                          {QStringLiteral("physics"), QJsonArray{physics}},
                          {QStringLiteral("mesh"), QJsonObject{{QStringLiteral("size"), QStringLiteral("normal")},
                                                               {QStringLiteral("custom"), true},
                                                               {QStringLiteral("hmax"), number(hAll)},
                                                               {QStringLiteral("features"), sizes}}}};
    // (With PMLs, twice the modes asked: the layers' own fast-dying modes -
    // above the core's index, losing nearly half their index a radian, all
    // their field in a layer - come first about the largest index, and are
    // left out below.)
    QJsonObject step{{QStringLiteral("type"), QStringLiteral("mode")},
                     {QStringLiteral("frequency"), QStringLiteral("%1[Hz]").arg(number(frequency))},
                     {QStringLiteral("modes"), layerNames.isEmpty() ? settings.modes : 2 * settings.modes},
                     {QStringLiteral("follow"), false},
                     {QStringLiteral("keeplayers"), true}};
    // A full-wave solve's PMLs in the cut: searched about the index whose
    // square is midway between the window's least and largest - the layers'
    // own modes (n² some 11 - 4.8j in a wire's oxide, Im n -0.7 to -1) then
    // further from it than the guided and the slowly dying ones (19 of 20
    // modes found about silicon's index were theirs).
    if (!settings.pmlDomains.empty() && !layerNames.isEmpty()) {
        double lo = 1e300, hi = 0;
        for (std::size_t i = 0; i < rects.size(); ++i)
            if (!isLayer[i]) {
                lo = std::min(lo, rectIndex[i]);
                hi = std::max(hi, rectIndex[i]);
            }
        if (hi > 0) step.insert(QStringLiteral("neff"), number(std::sqrt((lo * lo + hi * hi) / 2)));
    }
    QJsonObject json{{QStringLiteral("format"), QStringLiteral("qucs-s multiphysics 1")},
                     {QStringLiteral("components"), QJsonArray{component}},
                     {QStringLiteral("studies"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("study")}, {QStringLiteral("steps"), QJsonArray{step}}}}}};
    QString why;
    std::optional<Model> m2 = Model::fromJson(QJsonDocument(json).toJson(QJsonDocument::Compact), &why);
    if (!m2) {
        *error = tr("the cut's model: %1").arg(why);
        return out;
    }
    // The 3D model's definitions (its parameters and functions), and its
    // materials on the rectangles of their domains, in the same order.
    *m2->root.child(QStringLiteral("definitions")) = *model.root.child(QStringLiteral("definitions"));
    for (std::size_t i = 0; i < materials.size(); ++i) {
        QJsonArray names;
        for (std::size_t r = 0; r < rects.size(); ++r)
            if (picks[i].contains(rects[r].domain) && absorbKappa[r] == 0) names.append(QStringLiteral("r%1").arg(r + 1));
        if (names.isEmpty()) continue;
        Node copy = *materials[i];
        copy.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("objects"), names}});
        QString addWhy;
        if (!m2->add(m2->materials().tag, copy, -1, &addWhy)) {
            *error = tr("the cut's material %1: %2").arg(materials[i]->name(), addWhy);
            return out;
        }
    }
    // An absorbing wall's steps: their edge material's index at this
    // wavelength, and their extinction.
    {
        std::map<std::pair<double, double>, QJsonArray> byMedium;
        for (std::size_t r = 0; r < rects.size(); ++r)
            if (absorbKappa[r] > 0) byMedium[{rectIndex[r], absorbKappa[r]}].append(QStringLiteral("r%1").arg(r + 1));
        int k = 0;
        for (const auto& [medium, names] : byMedium) {
            Node a = m2->make(QStringLiteral("material"));
            a.label = QStringLiteral("absorber %1").arg(++k);
            a.props.insert(QStringLiteral("n"), number(medium.first));
            a.props.insert(QStringLiteral("kappa"), number(medium.second));
            a.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("objects"), names}});
            QString addWhy;
            if (!m2->add(m2->materials().tag, a, -1, &addWhy)) {
                *error = tr("the cut's absorbing wall: %1").arg(addWhy);
                return out;
            }
        }
    }
    // Solved before (the same model, the parameters it reads the same): as
    // it was.
    QByteArray key = m2->toJson();
    {
        const QString text = QString::fromUtf8(key);
        for (auto it = parameters->values.cbegin(); it != parameters->values.cend(); ++it) {
            const QRegularExpression own(QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(it.key())));
            if (own.match(text).hasMatch()) key += QByteArray("\n") + it.key().toUtf8() + '=' + QByteArray::number(it.value(), 'g', 17);
        }
        key += QByteArray("\nf=") + QByteArray::number(frequency, 'g', 17);
    }
    if (cachedCut(key, &out)) {
        out.section = section;
        return out;
    }
    // (Its geometry and mesh one cut at a time - quick beside the solve -, the
    // solves side by side.)
    GeometryBuild g;
    MeshBuild mb;
    {
        static std::mutex building;
        std::lock_guard<std::mutex> lock(building);
        g = buildGeometry(*m2, *parameters);
        if (g.ok()) mb = buildMesh(*m2, *parameters, g.topology);
    }
    if (!g.ok()) {
        *error = tr("the cut's geometry: %1").arg(QStringList(g.errors.values()).join(QLatin1String("; ")));
        return out;
    }
    if (!mb.mesh) {
        *error = tr("the cut's mesh: %1").arg(mb.error);
        return out;
    }
    Progress progress;
    if (settings.cancelled) progress = [c = settings.cancelled](double, const QString&) { return !c(); };
    // (With PMLs, as many again while too few are left of those asked - the
    // layers' own modes crowding the index searched about: 19 of a port's 20
    // in a wire's cut whose PMLs were a third of it -, to the step's 200.)
    int asked = layerNames.isEmpty() ? settings.modes : 2 * settings.modes;
    for (;;) {
        for (Node& study : m2->root.children)
            if (study.type == QLatin1String("study"))
                for (Node& st : study.children)
                    if (st.type == QLatin1String("mode")) st.props.insert(QStringLiteral("modes"), asked);
        out.beta.clear();
        out.te.clear();
        out.layers.clear();
        const StudyRun run = runStudy(*m2, parameters, g.topology, mb.mesh, QString(), progress);
        if (!run.ok()) {
            *error = run.error;
            return out;
        }
        std::shared_ptr<Solution> s = run.solutions.back();
        if (s->fields().empty() || !s->fields().front().edges) {
            *error = tr("the cut's mode analysis gave no field");
            return out;
        }
        const Field& f0 = s->fields().front();
        const int ne = f0.edges->size(), nn = f0.space->size();
        const int M = s->snapshotCount();
        out.Et = Eigen::MatrixXcd::Zero(ne, M);
        out.Es = Eigen::MatrixXcd::Zero(nn, M);
        for (int k = 0; k < M; ++k) {
            s->select(k);
            const Field& f = s->fields().front();
            if (int(f.values.size()) < 2 * ne + 2 * nn || f.extra.size() < 3) continue;
            cd beta(f.extra[0], f.extra[1]);
            double sign = 1;
            // Forward: decaying along +s, or propagating along it - a lossless
            // mode's imaginary part, the eigensolver's rounding (1e-16 of either
            // sign), taken for none: half the propagating modes were turned
            // backward, and a junction gave 4 % more power than it was fed.
            const double tol = 1e-10 * std::abs(beta);
            if (std::abs(beta.imag()) <= tol) beta = cd(beta.real(), 0);
            if (beta.imag() > 0 || (beta.imag() == 0 && beta.real() < 0)) {
                beta = -beta;
                sign = -1;   // (the backward mode of the one found: E_s turned over)
            }
            for (int i = 0; i < ne; ++i) out.Et(i, k) = cd(f.values[std::size_t(i)], f.values[std::size_t(ne + i)]);
            for (int i = 0; i < nn; ++i) out.Es(i, k) = sign * cd(f.values[std::size_t(2 * ne + i)], f.values[std::size_t(2 * ne + nn + i)]);
            out.beta.push_back(beta);
            const std::optional<double> te = s->global(QStringLiteral("emw.TEfraction"));
            out.te.push_back(te ? *te : NaN);
        }
        if (int(out.beta.size()) != M) {
            out.Et.conservativeResize(ne, int(out.beta.size()));
            out.Es.conservativeResize(nn, int(out.beta.size()));
        }
        // µr by triangle (H of curl E).
        out.mur.assign(s->mesh().triangles.size(), 1.0);
        {
            PointEvaluator ev(*s);
            for (int t = 0; t < int(s->mesh().triangles.size()); ++t) {
                ev.moveTo(t, 1.0 / 3, 1.0 / 3);
                const double mu = ev.coefficient(0, QStringLiteral("mur"));
                if (std::isfinite(mu) && mu > 0) out.mur[std::size_t(t)] = mu;
            }
        }
        out.bendRadius = section.bendRadius * topology.unitScale;
        out.bendCenter = section.bendCenter * topology.unitScale;
        // The PMLs: by triangle the layer it is in - its rectangle's box and its
        // material's k, as the mode solver's Media took them.
        out.layerOf.assign(s->mesh().triangles.size(), -1);
        {
            // (The box the PMLs surround: the other rectangles' - the window
            // where they are outside it -, as the mode solver's.)
            const double k = topology.unitScale;
            double box[4] = {1e300, -1e300, 1e300, -1e300};
            for (std::size_t i = 0; i < rects.size(); ++i) {
                if (isLayer[i]) continue;
                box[0] = std::min(box[0], rects[i].u0);
                box[1] = std::max(box[1], rects[i].u1);
                box[2] = std::min(box[2], rects[i].z0);
                box[3] = std::max(box[3], rects[i].z1);
            }
            if (!(box[0] < box[1])) {
                box[0] = section.u0;
                box[1] = section.u1;
                box[2] = section.z0;
                box[3] = section.z1;
            }
            for (int a = 0; a < 4; ++a) out.inner[a] = box[a] * k;
            out.pmlLog = -std::log(settings.pmlReflection);
            std::vector<int> layerOfRect(rects.size(), -1);
            for (std::size_t i = 0; i < rects.size(); ++i) {
                if (!isLayer[i]) continue;
                EmeModes::Layer L;
                L.outer[0] = rects[i].u0 * k;
                L.outer[1] = rects[i].u1 * k;
                L.outer[2] = rects[i].z0 * k;
                L.outer[3] = rects[i].z1 * k;
                L.k = out.k0 * std::sqrt(std::max(1.0, rectIndex[i] * rectIndex[i]));
                layerOfRect[i] = int(out.layers.size());
                out.layers.push_back(L);
            }
            // (By triangle the PML or absorbing wall it is in.)
            out.inAbsorber.assign(s->mesh().triangles.size(), 0);
            const bool anyAbsorber = std::any_of(absorbKappa.begin(), absorbKappa.end(), [](double k) { return k > 0; });
            if (!out.layers.empty() || anyAbsorber) {
                const Mesh& mesh = s->mesh();
                for (int t = 0; t < int(mesh.triangles.size()); ++t) {
                    const auto& tv = mesh.triangles[std::size_t(t)];
                    const QPointF c = (mesh.nodes[std::size_t(tv[0])] + mesh.nodes[std::size_t(tv[1])] + mesh.nodes[std::size_t(tv[2])]) / 3.0;
                    for (std::size_t i = 0; i < rects.size(); ++i) {
                        if (!(c.x() >= rects[i].u0 && c.x() <= rects[i].u1 && c.y() >= rects[i].z0 && c.y() <= rects[i].z1)) continue;
                        if (isLayer[i]) out.layerOf[std::size_t(t)] = layerOfRect[i];
                        if (absorbKappa[i] > 0) out.inAbsorber[std::size_t(t)] = 1;
                        if (isLayer[i] || absorbKappa[i] > 0) break;
                    }
                }
            }
        }
        out.solution = s;
        normalize(out);
        // With PMLs: the layers' own fast-dying modes left out (nearly all their
        // field in a layer, |Im n| over a tenth of Re n) - they carry nothing in
        // the window, and took 7 of 30 modes of a wire's cut - and the modes kept
        // no more than asked, those that carry radiation (in a layer but
        // dying slowly) kept with the window's.
        if (!out.layers.empty()) {
            std::vector<int> kept;
            for (int m = 0; m < out.count(); ++m) {
                const cd n = out.neff(m);
                const bool spurious = out.inLayers[std::size_t(m)] > 0.95 && std::abs(n.imag()) > 0.1 * std::abs(n.real());
                if (!spurious) kept.push_back(m);
            }
            if (int(kept.size()) > settings.modes) kept.resize(std::size_t(settings.modes));
            keepModes(out, kept);
        }
        if (out.layers.empty() || out.count() >= settings.modes || asked >= 200) break;
        asked = std::min(200, 2 * asked);
    }
    if (out.count() == 0) {
        *error = tr("the cut's modes are none that can be normalized");
        return out;
    }
    keepCut(key, out);
    return out;
}

void clearEmeCache()
{
    std::lock_guard<std::mutex> lock(cutCacheMutex);
    cutCache().clear();
    cutCacheBytes = 0;
}

// ----------------------------------------------------------- the S-matrices

SMatrix identityS(int n)
{
    SMatrix s;
    s.s11 = Eigen::MatrixXcd::Zero(n, n);
    s.s22 = Eigen::MatrixXcd::Zero(n, n);
    s.s12 = Eigen::MatrixXcd::Identity(n, n);
    s.s21 = Eigen::MatrixXcd::Identity(n, n);
    return s;
}

SMatrix interfaceS(const Eigen::MatrixXcd& O)
{
    const int n1 = int(O.rows()), n2 = int(O.cols());
    const Eigen::MatrixXcd I1 = Eigen::MatrixXcd::Identity(n1, n1), I2 = Eigen::MatrixXcd::Identity(n2, n2);
    const Eigen::MatrixXcd OOt = O * O.transpose(), OtO = O.transpose() * O;
    const Eigen::PartialPivLU<Eigen::MatrixXcd> lu1(I1 + OOt), lu2(I2 + OtO);
    SMatrix s;
    s.s11 = lu1.solve(I1 - OOt);
    s.s21 = lu2.solve(2.0 * O.transpose());
    s.s22 = -lu2.solve(I2 - OtO);
    s.s12 = lu1.solve(2.0 * O);
    return s;
}

SMatrix junctionS(const Eigen::MatrixXcd& ab, const Eigen::MatrixXcd& ba)
{
    // E on the right's h and H on the left's e; and the other way - the
    // interface seen from the right, turned round.
    SMatrix s = interfaceS(ab);
    const SMatrix back = interfaceS(ba);
    s.s11 = 0.5 * (s.s11 + back.s22);
    s.s22 = 0.5 * (s.s22 + back.s11);
    s.s12 = 0.5 * (s.s12 + back.s21);
    s.s21 = 0.5 * (s.s21 + back.s12);
    return s;
}

SMatrix propagationS(const std::vector<cd>& beta, double length)
{
    const int n = int(beta.size());
    SMatrix s;
    s.s11 = Eigen::MatrixXcd::Zero(n, n);
    s.s22 = Eigen::MatrixXcd::Zero(n, n);
    Eigen::VectorXcd p(n);
    for (int i = 0; i < n; ++i) p(i) = std::exp(-J * beta[std::size_t(i)] * length);
    s.s12 = p.asDiagonal();
    s.s21 = p.asDiagonal();
    return s;
}

SMatrix cascade(const SMatrix& a, const SMatrix& b)
{
    // a: left -> middle, b: middle -> right. X = (I - b11 a22)⁻¹, Y = (I - a22 b11)⁻¹.
    const int m = int(a.s22.rows());
    const Eigen::MatrixXcd I = Eigen::MatrixXcd::Identity(m, m);
    const Eigen::PartialPivLU<Eigen::MatrixXcd> X(I - b.s11 * a.s22), Y(I - a.s22 * b.s11);
    const Eigen::MatrixXcd Xb12 = X.solve(b.s12);
    SMatrix s;
    s.s11 = a.s11 + a.s12 * X.solve(b.s11 * a.s21);
    s.s12 = a.s12 * Xb12;
    s.s21 = b.s21 * Y.solve(a.s21);
    s.s22 = b.s22 + b.s21 * (a.s22 * Xb12);
    return s;
}

SMatrix powerS(const SMatrix& s, long long n)
{
    // (Powers of one S-matrix: the star product is associative and they
    // commute, so by its binary digits - s, s², s⁴... - those wanted.)
    SMatrix result = identityS(s.left()), base = s;
    for (bool first = true; n > 0; n >>= 1) {
        if (!first) base = cascade(base, base);
        first = false;
        if (n & 1) result = cascade(result, base);
    }
    return result;
}

// ---------------------------------------------------------------- the cells

namespace {

/// Where the device's axis or path is at s: its point, its direction, the
/// cut's direction (a quarter left of it), and on an arc the bend's radius
/// and its axis's u (+R turning left, -R turning right).
struct Frame {
    QPointF point, along, across;
    double bendRadius = 0, bendCenter = 0;
    int piece = -1;
};

Frame frameAt(const EmeDevice& d, double s)
{
    Frame f;
    if (d.path.empty()) {
        f.point = d.origin + d.along * s;
        f.along = d.along;
        f.across = d.across;
        return f;
    }
    double from = 0;
    for (int i = 0; i < int(d.path.size()); ++i) {
        const EmePiece& p = d.path[std::size_t(i)];
        if (s <= from + p.length || i + 1 == int(d.path.size())) {
            const double t = std::clamp(s - from, 0.0, p.length);
            if (!p.arc) {
                f.point = p.start + p.dir * t;
                f.along = p.dir;
            } else {
                const double sign = p.sweep >= 0 ? 1.0 : -1.0;
                const double phi = p.angle0 + sign * t / p.radius;
                f.point = p.center + QPointF(std::cos(phi), std::sin(phi)) * p.radius;
                f.along = sign > 0 ? QPointF(-std::sin(phi), std::cos(phi)) : QPointF(std::sin(phi), -std::cos(phi));
                f.bendRadius = p.radius;
                f.bendCenter = sign * p.radius;
            }
            f.across = QPointF(-f.along.y(), f.along.x());
            f.piece = i;
            return f;
        }
        from += p.length;
    }
    return f;
}

/// The cut of \a device at s along its axis or path - a bend's on an arc.
CrossSection cutAt(const Topology3& topology, const EmeDevice& device, double s, QString* error)
{
    const Frame f = frameAt(device, s);
    CrossSection cs = cutAcross(topology, f.point, f.across, device.u0, device.u1, device.z0, device.z1, error);
    cs.bendRadius = f.bendRadius;
    cs.bendCenter = f.bendCenter;
    return cs;
}

} // namespace

std::vector<EmeCell> emeCells(const Topology3& topology, const EmeDevice& device, std::vector<CrossSection>* sections, QString* error,
                              const std::vector<int>* materialOf)
{
    std::vector<EmeCell> cells;
    sections->clear();
    if (!topology.plan || !(device.s1 > device.s0)) {
        *error = tr("the expansion's axis has no length");
        return cells;
    }
    const Topology& plan = *topology.plan;
    const double span = device.s1 - device.s0;
    const double tol = 1e-9 * std::max({1.0, std::abs(device.s0), std::abs(device.s1)});
    const double utol = 1e-9 * std::max({1.0, std::abs(device.u0), std::abs(device.u1)});
    // Where the cut may change along a straight line (from origin along
    // `along`, s from sFrom to sTo there, \a offset more in the device's s):
    // the plan's vertices across the window, where its boundaries cross the
    // window's sides, an arc's extreme across it (a bend's outer edge).
    std::vector<double> ss{device.s0, device.s1};
    auto straightBreaks = [&](QPointF origin, QPointF along, QPointF across, double sFrom, double sTo, double offset) {
        auto sOf = [&](QPointF p) { return (p.x() - origin.x()) * along.x() + (p.y() - origin.y()) * along.y(); };
        auto uOf = [&](QPointF p) { return (p.x() - origin.x()) * across.x() + (p.y() - origin.y()) * across.y(); };
        auto keep = [&](double s) {
            if (s >= sFrom - tol && s <= sTo + tol) ss.push_back(offset + s);
        };
        for (const QPointF& v : plan.vertices) {
            const double u = uOf(v);
            if (u >= device.u0 - utol && u <= device.u1 + utol) keep(sOf(v));
        }
        for (double side : {device.u0, device.u1}) {
            std::vector<double> at;
            // (The window's side is the line through origin + across·side along +s.)
            const QPointF O = origin + across * side;
            for (const Topology::Segment& seg : plan.segments) crossSegment(plan, seg, O, along, across, at);
            for (double s : at) keep(s);
        }
        for (const Curve& c : plan.curves) {
            if (c.kind != Curve::Arc) continue;
            // (c + a cos t + b sin t)·along has its extremes where -a·along sin t + b·along cos t = 0.)
            const double aa = c.a.x() * along.x() + c.a.y() * along.y(), ba = c.b.x() * along.x() + c.b.y() * along.y();
            const double t0 = std::atan2(ba, aa);
            for (double t : {t0, t0 + M_PI}) {
                const double lo = std::min(c.t0, c.t1), hi = std::max(c.t0, c.t1);
                const double tt = t + 2 * M_PI * std::ceil((lo - t) / (2 * M_PI));
                if (tt > hi) continue;
                const QPointF p = c.at(tt);
                const double u = uOf(p);
                if (u >= device.u0 - utol && u <= device.u1 + utol) keep(sOf(p));
            }
        }
    };
    if (device.path.empty()) straightBreaks(device.origin, device.along, device.across, device.s0, device.s1, 0);
    else {
        // A path: each straight piece as an axis of its own; an arc's ends
        // (within, its cut is the same all along where its geometry is
        // concentric with it - one cell of the bend's modes -, else in steps).
        double from = 0;
        // (What a cut reaches toward a bend's axis: the window's inner side
        // and its walls' layer - an absorbing wall's or a PML's.)
        const QString& sides = device.settings.sides;
        const double layer = sides == QLatin1String("absorbing") ? device.settings.absorberThickness
                             : sides == QLatin1String("pml")     ? device.settings.pmlThickness
                                                                 : 0.0;
        for (const EmePiece& p : device.path) {
            ss.push_back(from);
            ss.push_back(from + p.length);
            if (!p.arc) straightBreaks(p.start, p.dir, QPointF(-p.dir.y(), p.dir.x()), 0, p.length, from);
            else {
                // (Turning left its axis is on the left, +u; turning right, -u.)
                const double inner = (p.sweep > 0 ? device.u1 : -device.u0) + layer;
                if (p.radius <= inner + utol) {
                    *error = tr("a bend of the path is %1 in radius, its cut reaching %2 toward its axis (the window's inner side%3): the cut "
                                "must stay within the bend's radius - a window narrower on the bend's inner side, or a thinner wall")
                                 .arg(formatNumber(p.radius, 4), formatNumber(inner, 4), layer > 0 ? tr(" and its wall's layer") : QString());
                    return cells;
                }
            }
            from += p.length;
        }
    }
    // (A periodic section's ends: no cell crosses them.)
    for (const EmePeriodic& p : device.periodic) {
        ss.push_back(p.s0);
        ss.push_back(p.s1);
    }
    auto periodEnd = [&](double s) {
        for (const EmePeriodic& p : device.periodic)
            if (std::abs(s - p.s0) <= tol || std::abs(s - p.s1) <= tol) return true;
        return false;
    };
    std::sort(ss.begin(), ss.end());
    std::vector<double> breaks;
    for (double s : ss) {
        if (s < device.s0 - tol || s > device.s1 + tol) continue;
        const double v = std::clamp(s, device.s0, device.s1);
        if (breaks.empty() || v - breaks.back() > tol) breaks.push_back(v);
    }
    auto sectionOf = [&](const CrossSection& cs) {
        const double ctol = 1e-9 * std::max({1.0, std::abs(device.u0), std::abs(device.u1), std::abs(device.z0), std::abs(device.z1)});
        for (std::size_t i = 0; i < sections->size(); ++i)
            if ((*sections)[i].sameAs(cs, ctol, materialOf)) return int(i);
        sections->push_back(cs);
        return int(sections->size()) - 1;
    };
    for (std::size_t i = 0; i + 1 < breaks.size(); ++i) {
        const double a = breaks[i], b = breaks[i + 1];
        if (b - a <= tol) continue;
        const double in = std::min(1e-6 * (b - a), 1e-6 * span);
        QString why;
        const CrossSection left = cutAt(topology, device, a + in, &why), mid = cutAt(topology, device, (a + b) / 2, &why),
                           right = cutAt(topology, device, b - in, &why);
        if (!why.isEmpty()) {
            *error = why;
            cells.clear();
            return cells;
        }
        const double ctol = 1e-7 * std::max({1.0, std::abs(device.u1 - device.u0), std::abs(device.z1 - device.z0)});
        const bool uniform = left.sameAs(mid, ctol, materialOf) && right.sameAs(mid, ctol, materialOf);
        int n = 1;
        if (!uniform) n = device.maxCell > 0 ? std::max(2, int(std::ceil((b - a) / device.maxCell - 1e-9))) : 8;
        for (int k = 0; k < n; ++k) {
            const double c0 = a + (b - a) * k / n, c1 = a + (b - a) * (k + 1) / n;
            const CrossSection cs = n == 1 ? mid : cutAt(topology, device, (c0 + c1) / 2, &why);
            if (!why.isEmpty()) {
                *error = why;
                cells.clear();
                return cells;
            }
            const int sec = sectionOf(cs);
            // (Cells of the same cut one after another made one - not across a
            // periodic section's end.)
            if (!cells.empty() && cells.back().section == sec && !periodEnd(c0)) cells.back().s1 = c1;
            else cells.push_back({c0, c1, sec});
        }
    }
    if (cells.empty()) *error = tr("the expansion's axis crosses no solid");
    return cells;
}

// ------------------------------------------------------------- the device

EmeResult solveEme(const Model& model, std::shared_ptr<ParameterScope> parameters, const Topology3& topology, const EmeDevice& device,
                   double frequency)
{
    EmeResult r;
    r.frequency = frequency;
    QElapsedTimer timer;
    timer.start();
    if (!parameters) parameters = evaluateParameters(model);
    std::vector<CrossSection> sections;
    QString why;
    // (Cuts alike in their rectangles and those rectangles' materials are one
    // cut, its modes and junctions solved once: a drawn grating's teeth, each
    // a domain of its own, two cuts and two junctions.)
    std::vector<int> materialOf;
    {
        const std::vector<const Node*> of = domainMaterials(model, topology);
        std::map<const Node*, int> classes;
        for (std::size_t d = 0; d < of.size(); ++d)
            materialOf.push_back(of[d] ? classes.emplace(of[d], int(classes.size())).first->second : -1 - int(d));
    }
    r.cells = emeCells(topology, device, &sections, &why, &materialOf);
    if (r.cells.empty()) {
        r.error = why;
        return r;
    }
    r.sections = int(sections.size());
    // The ports: at each end, one over the whole window - the end cell's own
    // modes - or boxes of it, each its own cut's modes.
    std::vector<int> atStart, atEnd;
    for (int p = 0; p < int(device.ports.size()); ++p) (device.ports[std::size_t(p)].atEnd ? atEnd : atStart).push_back(p);
    for (const std::vector<int>* end : {&atStart, &atEnd}) {
        int whole = 0;
        for (int p : *end) whole += device.ports[std::size_t(p)].whole ? 1 : 0;
        if (whole > 0 && end->size() > 1) {
            r.error = tr("an end of the expansion has a port over its whole window and another: each port at an end with two or more "
                         "takes a box of the window (its waveguide's)");
            return r;
        }
    }
    // Every distinct cut's modes, side by side on the machine's threads.
    std::vector<EmeModes> modes(sections.size());
    std::vector<QString> errors(sections.size());
    // Ports in boxes: their cuts too (at the end cell's middle, within the
    // box), solved with the others.
    struct BoxPort {
        int port;
        CrossSection cs;
        EmeModes modes;
        QString error;
    };
    std::vector<BoxPort> boxes;
    for (int p = 0; p < int(device.ports.size()); ++p) {
        const EmePort& port = device.ports[std::size_t(p)];
        if (port.whole) continue;
        const EmeCell& cell = port.atEnd ? r.cells.back() : r.cells.front();
        const double s = (cell.s0 + cell.s1) / 2;
        QString cutWhy;
        const Frame f = frameAt(device, s);
        CrossSection cs = cutAcross(topology, f.point, f.across, port.u0, port.u1, port.z0, port.z1, &cutWhy);
        cs.bendRadius = f.bendRadius;
        cs.bendCenter = f.bendCenter;
        if (!cutWhy.isEmpty()) {
            r.error = tr("port %1: %2").arg(port.name, cutWhy);
            return r;
        }
        boxes.push_back({p, cs, {}, {}});
    }
    // (Its stages timed for the log: the cuts, the junctions' overlaps, the
    // cascade.)
    QElapsedTimer stage;
    stage.start();
    qint64 cutsMs = 0, overlapsMs = 0;
    std::mutex telling;
    // (The cuts reported as the first 60 % of the solve, the overlaps the rest.)
    auto tell = [&](double f, const QString& what) {
        if (!device.settings.progress) return;
        std::lock_guard<std::mutex> lock(telling);
        device.settings.progress(f, what);
    };
    {
        const int jobs = int(sections.size() + boxes.size());
        std::atomic<int> next{0}, done{0};
        auto work = [&] {
            for (int j = next++; j < jobs; j = next++) {
                if (device.settings.cancelled && device.settings.cancelled()) return;
                if (j < int(sections.size())) {
                    modes[std::size_t(j)] = solveSectionModes(model, parameters, topology, sections[std::size_t(j)], frequency, device.settings,
                                                             &errors[std::size_t(j)]);
                } else {
                    BoxPort& b = boxes[std::size_t(j) - sections.size()];
                    EmeSettings st = device.settings;
                    // (A port's box its own guided mode's: metal round it, whatever
                    // the device's walls - a PML there would lie over the device.)
                    st.sides = st.floors = QStringLiteral("pec");
                    // (A port's few modes: those it takes, and some more to tell TE from TM.)
                    int asked = 0;
                    for (const QString& l : device.ports[std::size_t(b.port)].modes) {
                        bool num = false;
                        const int n = l.toInt(&num);
                        asked = std::max(asked, num ? n : l.mid(2).toInt() + 1);
                    }
                    st.modes = std::max(6, 3 * asked + 2);
                    b.modes = solveSectionModes(model, parameters, topology, b.cs, frequency, st, &b.error);
                }
                const int finished = ++done;
                tell(0.6 * finished / jobs, tr("cuts %1 of %2").arg(finished).arg(jobs));
            }
        };
        const int threads = std::clamp(device.settings.threads > 0 ? device.settings.threads : int(std::thread::hardware_concurrency()), 1,
                                       std::max(1, jobs));
        std::vector<std::thread> pool;
        for (int t = 1; t < threads; ++t) pool.emplace_back(work);
        work();
        for (std::thread& t : pool) t.join();
    }
    cutsMs = stage.restart();
    if (device.settings.cancelled && device.settings.cancelled()) {
        r.error = tr("cancelled");
        return r;
    }
    for (std::size_t i = 0; i < sections.size(); ++i)
        if (!errors[i].isEmpty() || modes[i].count() == 0) {
            const double s = [&] {
                for (const EmeCell& c : r.cells)
                    if (c.section == int(i)) return (c.s0 + c.s1) / 2;
                return NaN;
            }();
            r.error = tr("the cut at s = %1: %2").arg(formatNumber(s, 6), errors[i].isEmpty() ? tr("no mode found") : errors[i]);
            return r;
        }
    for (const BoxPort& b : boxes)
        if (!b.error.isEmpty() || b.modes.count() == 0) {
            r.error = tr("port %1: %2").arg(device.ports[std::size_t(b.port)].name, b.error.isEmpty() ? tr("no mode found") : b.error);
            return r;
        }
    // A cut whose modes kept all propagate: what a junction scatters into
    // the window's other propagating modes has nowhere to go but back into
    // these - its S-parameters too large (a wire's junction in a window of
    // 4 by 3.2 µm, some 70 propagating modes: |T|² 0.918 with 20 modes, 0.902
    // with 40). Said with the window's count by Weyl's law, A k0² n²/2π.
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const EmeModes& m = modes[i];
        const cd last = m.beta.back();
        if (!(std::abs(last.imag()) <= 1e-9 * std::abs(last) && last.real() > 0)) continue;
        double s = NaN;
        for (const EmeCell& c : r.cells)
            if (c.section == int(i)) {
                s = (c.s0 + c.s1) / 2;
                break;
            }
        const QString w = tr("the cut at s = %1 keeps %2 modes, all of them propagating (the last at an index of %3): what a junction scatters "
                             "is not all carried, its S-parameters too large - ask for more modes (its window has some %4 propagating), or a "
                             "smaller window")
                              .arg(formatNumber(s, 4))
                              .arg(m.count())
                              .arg(formatNumber(last.real() / m.k0, 4))
                              .arg(int(std::ceil(std::max(m.propagating, double(m.count())) / 10.0) * 10));
        if (!r.warnings.contains(w)) r.warnings << w;
    }
    // The cells cascaded: each carried along, each change of cut an
    // interface (the overlaps of a pair of cuts kept: a drawn grating's
    // thousand teeth two pairs), a periodic section's cells one period - its
    // S-matrix with the junction back to its start raised to all its periods
    // but the last by squaring, then the last.
    const double stol = 1e-9 * std::max({1.0, std::abs(device.s0), std::abs(device.s1)});
    // Every overlap the cascade and the ports want - each distinct junction
    // (two cells one after another, a periodic section's last cell back to
    // its first) and each boxed port's projection - side by side on the
    // solve's threads, before the cascade: the cascade's own work is small
    // matrices, the overlaps all but all of a solve after its cuts.
    std::vector<std::pair<int, int>> pairs;
    {
        std::map<std::pair<int, int>, int> seen;
        auto need = [&](int a, int b) {
            if (a != b && seen.emplace(std::make_pair(a, b), 0).second) pairs.push_back({a, b});
        };
        for (std::size_t c = 1; c < r.cells.size(); ++c) need(r.cells[c - 1].section, r.cells[c].section);
        for (const EmePeriodic& p : device.periodic) {
            if (p.periods < 2) continue;
            std::size_t c0 = r.cells.size(), c1 = 0;
            for (std::size_t c = 0; c < r.cells.size(); ++c)
                if (r.cells[c].s0 >= p.s0 - stol && r.cells[c].s1 <= p.s1 + stol) {
                    c0 = std::min(c0, c);
                    c1 = c;
                }
            if (c0 < r.cells.size()) need(r.cells[c1].section, r.cells[c0].section);
        }
    }
    const EmeModes& first = modes[std::size_t(r.cells.front().section)];
    const EmeModes& last = modes[std::size_t(r.cells.back().section)];
    std::vector<std::pair<const EmeModes*, const EmeModes*>> overlapPairs;
    for (const auto& [a, b] : pairs) overlapPairs.push_back({&modes[std::size_t(a)], &modes[std::size_t(b)]});
    for (const BoxPort& b : boxes) overlapPairs.push_back({&b.modes, device.ports[std::size_t(b.port)].atEnd ? &last : &first});
    std::vector<Eigen::MatrixXcd> overlapAB, overlapBA;
    const bool overlapsDone = modeOverlapsMany(
        overlapPairs, device.settings.threads, device.settings.cancelled,
        [&](int done, int total) { tell(0.6 + 0.4 * done / std::max(1, total), tr("junctions' overlaps %1 of %2").arg(done).arg(total)); }, &overlapAB,
        &overlapBA);
    overlapsMs = stage.restart();
    if (!overlapsDone || (device.settings.cancelled && device.settings.cancelled())) {
        r.error = tr("cancelled");
        return r;
    }
    std::map<std::pair<int, int>, SMatrix> interfaces;
    for (std::size_t i = 0; i < pairs.size(); ++i) interfaces.emplace(pairs[i], junctionS(overlapAB[i], overlapBA[i]));
    auto junction = [&](int a, int b) -> const SMatrix& {
        auto it = interfaces.find({a, b});
        if (it == interfaces.end()) it = interfaces.emplace(std::make_pair(a, b), identityS(modes[std::size_t(a)].count())).first;   // (a == b)
        return it->second;
    };
    auto carried = [&](std::size_t c) {
        const EmeCell& cell = r.cells[c];
        return propagationS(modes[std::size_t(cell.section)].beta, (cell.s1 - cell.s0) * topology.unitScale);
    };
    // (The cells from first to last, with the interfaces between them.)
    auto run = [&](std::size_t first, std::size_t last) {
        SMatrix s = carried(first);
        for (std::size_t c = first + 1; c <= last; ++c) s = cascade(cascade(s, junction(r.cells[c - 1].section, r.cells[c].section)), carried(c));
        return s;
    };
    SMatrix total;
    long long repeated = 0;
    std::map<std::pair<int, int>, long long> uses;   // each junction's, the periods' with them
    for (std::size_t c = 0, previous = 0; c < r.cells.size();) {
        std::size_t last = c;
        long long periods = 1;
        for (const EmePeriodic& p : device.periodic)
            if (std::abs(r.cells[c].s0 - p.s0) <= stol) {
                while (last + 1 < r.cells.size() && r.cells[last + 1].s1 <= p.s1 + stol) ++last;
                periods = p.periods;
                break;
            }
        SMatrix block = run(c, last);
        for (std::size_t k = c + 1; k <= last; ++k) uses[{r.cells[k - 1].section, r.cells[k].section}] += periods;
        if (periods > 1) {
            const SMatrix period = cascade(block, junction(r.cells[last].section, r.cells[c].section));
            block = cascade(powerS(period, periods - 1), block);
            repeated += periods;
            uses[{r.cells[last].section, r.cells[c].section}] += periods - 1;
        }
        if (c > 0) ++uses[{r.cells[previous].section, r.cells[c].section}];
        total = c == 0 ? block : cascade(cascade(total, junction(r.cells[previous].section, r.cells[c].section)), block);
        previous = last;
        c = last + 1;
    }
    // The ports' modes: each a projection of the end cut's modes - what an
    // end cell's outgoing field leaves in it, and what of each end mode it
    // feeds - ½(⟨e_p, h_n⟩ + ⟨e_n, h_p⟩), the same both ways: S stays
    // reciprocal. A whole window's port takes its modes as they are. (Their
    // overlaps among those computed above, after the junctions'.)
    const qint64 cascadeMs = stage.restart();
    std::vector<Eigen::RowVectorXcd> projL, projR;   // by port mode: over the end cut's modes
    for (int p = 0; p < int(device.ports.size()); ++p) {
        const EmePort& port = device.ports[std::size_t(p)];
        const EmeModes& end = port.atEnd ? last : first;
        const EmeModes* own = &end;
        Eigen::MatrixXcd projection;   // own modes by end modes
        if (!port.whole) {
            for (std::size_t k = 0; k < boxes.size(); ++k)
                if (boxes[k].port == p) {
                    own = &boxes[k].modes;
                    const std::size_t i = pairs.size() + k;
                    projection = 0.5 * (overlapAB[i] + overlapBA[i].transpose());
                }
        }
        for (const QString& l : port.modes) {
            const int k = own->find(l);
            if (k < 0) {
                QStringList found;
                for (int i = 0; i < std::min(own->count(), 8); ++i)
                    found << QStringLiteral("%1 (%2)").arg(own->label(i), formatNumber(own->neff(i).real(), 5));
                r.error = tr("port %1 has no mode %2: its cut's modes are %3").arg(port.name, l, found.join(QLatin1String(", ")));
                return r;
            }
            EmeResult::PortMode pm;
            pm.port = p;
            pm.label = own->label(k);
            pm.neff = own->neff(k);
            pm.te = own->te[std::size_t(k)];
            r.portModes.push_back(pm);
            Eigen::RowVectorXcd row = Eigen::RowVectorXcd::Zero(end.count());
            if (port.whole) row(k) = 1;
            else row = projection.row(k);
            (port.atEnd ? projR : projL).push_back(row);
        }
    }
    // S between the port modes: [left; right] of the cells' S.
    const int nL = int(projL.size()), nR = int(projR.size()), N = nL + nR;
    Eigen::MatrixXcd QL = Eigen::MatrixXcd::Zero(nL, first.count()), QR = Eigen::MatrixXcd::Zero(nR, last.count());
    for (int i = 0; i < nL; ++i) QL.row(i) = projL[std::size_t(i)];
    for (int i = 0; i < nR; ++i) QR.row(i) = projR[std::size_t(i)];
    Eigen::MatrixXcd S = Eigen::MatrixXcd::Zero(N, N);
    if (nL > 0) S.block(0, 0, nL, nL) = QL * total.s11 * QL.transpose();
    if (nL > 0 && nR > 0) {
        S.block(0, nL, nL, nR) = QL * total.s12 * QR.transpose();
        S.block(nL, 0, nR, nL) = QR * total.s21 * QL.transpose();
    }
    if (nR > 0) S.block(nL, nL, nR, nR) = QR * total.s22 * QR.transpose();
    // In the model's order of ports: left ones first came from the loop's
    // order; put each port mode at its own place.
    std::vector<int> order;   // S's row of each port mode in r.portModes' order
    {
        int li = 0, ri = 0;
        for (const EmeResult::PortMode& pm : r.portModes) order.push_back(device.ports[std::size_t(pm.port)].atEnd ? nL + ri++ : li++);
    }
    r.S = Eigen::MatrixXcd::Zero(N, N);
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) r.S(i, j) = S(order[std::size_t(i)], order[std::size_t(j)]);
    r.balance.assign(std::size_t(N), 0.0);
    for (int j = 0; j < N; ++j) r.balance[std::size_t(j)] = r.S.col(j).squaredNorm();
    const int threads = std::max(1, device.settings.threads > 0 ? device.settings.threads : int(std::thread::hardware_concurrency()));
    r.log << tr("Eigenmode expansion at %1: %2 cells, %3 distinct cuts of %4 modes, %5 port modes; %6 s - the cuts %7 s, the overlaps %8 s "
                "(%9 of them), the cascade %10 s, on %11 threads")
                 .arg(formatQuantity(C0 / frequency, Dim::length()))
                 .arg(r.cells.size())
                 .arg(sections.size())
                 .arg(device.settings.modes)
                 .arg(N)
                 .arg(timer.elapsed() / 1000.0, 0, 'f', 2)
                 .arg(cutsMs / 1000.0, 0, 'f', 2)
                 .arg(overlapsMs / 1000.0, 0, 'f', 2)
                 .arg(overlapPairs.size())
                 .arg((cascadeMs + stage.elapsed()) / 1000.0, 0, 'f', 2)
                 .arg(threads);
    if (repeated > 0) r.log << tr("Its periodic sections: %1 periods in all, each drawn once").arg(repeated);
    // A junction that recurs - a grating's, drawn or a periodic section's:
    // its stopband sits where the cuts' indices put it, L (n1 + n2) = λ, and
    // its reflection is its steps', a few nanometres no cut alone sees; both
    // want a fine mesh. (A 2D grating against the full-wave solver: at 15
    // elements per wavelength its stopband 0.4 nm short, at 40 within 2e-4
    // of its reflection; a silicon wire's sidewall grating's peak 0.243 at
    // 20, 0.279 at 40.)
    {
        std::pair<int, int> most{-1, -1};
        long long count = 0;
        for (const auto& [pair, n] : uses)
            if (pair.first != pair.second && n > count) {
                most = pair;
                count = n;
            }
        if (count >= 10 && device.settings.resolution < 30) {
            auto at = [&](int section) {
                for (const EmeCell& c : r.cells)
                    if (c.section == section) return (c.s0 + c.s1) / 2;
                return NaN;
            };
            const QString w = tr("the junction of the cuts at s = %1 and %2 recurs %3 times - a grating's: its stopband sits where the cuts' "
                                 "indices put it and its reflection is its steps', both wanting some 40 elements per wavelength (this solve's "
                                 "%4) - at 20, a silicon wire's sidewall grating reflected 13 % less than at 40")
                                  .arg(formatNumber(at(most.first), 4), formatNumber(at(most.second), 4))
                                  .arg(count)
                                  .arg(formatNumber(device.settings.resolution, 3));
            if (!r.warnings.contains(w)) r.warnings << w;
        }
    }
    return r;
}

// ------------------------------------------------------------- Touchstone

QString emeTouchstone(const EmeDevice& device, const std::vector<EmeResult>& results, const QStringList& comments, int* ports, QString* error)
{
    if (results.empty()) {
        if (error) *error = tr("nothing is solved");
        return {};
    }
    const EmeResult& first = results.front();
    const int N = int(first.portModes.size());
    for (const EmeResult& r : results)
        if (int(r.portModes.size()) != N || r.S.rows() != N) {
            if (error) *error = tr("the wavelengths' port modes differ in number");
            return {};
        }
    if (N == 0) {
        if (error) *error = tr("there is no port mode");
        return {};
    }
    if (ports) *ports = N;
    auto num = [](double v) { return QString::number(v, 'g', 12); };
    QString out;
    for (const QString& c : comments)
        for (const QString& line : c.split(QLatin1Char('\n'))) out += QStringLiteral("! ") + line + QLatin1Char('\n');
    out += QStringLiteral("! Optical S-parameters by eigenmode expansion: each port an optical port's mode, normalized to 1 W - the reference "
                          "impedance below a placeholder, the same at every port\n");
    for (int i = 0; i < N; ++i) {
        const EmeResult::PortMode& pm = first.portModes[std::size_t(i)];
        if (device.fullWave)
            out += QStringLiteral("! Port %1: optical port %2's %3 (neff %4 at %5)\n")
                       .arg(i + 1)
                       .arg(device.ports[std::size_t(pm.port)].name, pm.label, formatNumber(pm.neff.real(), 6), formatQuantity(C0 / first.frequency, Dim::length()));
        else
            out += QStringLiteral("! Port %1: optical port %2's %3, at the %4 of the device (neff %5 at %6)\n")
                       .arg(i + 1)
                       .arg(device.ports[std::size_t(pm.port)].name, pm.label, device.ports[std::size_t(pm.port)].atEnd ? QStringLiteral("end") : QStringLiteral("start"),
                            formatNumber(pm.neff.real(), 6), formatQuantity(C0 / first.frequency, Dim::length()));
    }
    out += QStringLiteral("# Hz S RI R 50\n");
    std::vector<int> order(results.size());
    for (std::size_t k = 0; k < order.size(); ++k) order[k] = int(k);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return results[std::size_t(a)].frequency < results[std::size_t(b)].frequency; });
    for (int k : order) {
        const EmeResult& r = results[std::size_t(k)];
        // Two ports: S11 S21 S12 S22; else by rows, four to a line.
        std::vector<std::pair<int, int>> cells;
        if (N == 2) cells = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
        else
            for (int i = 0; i < N; ++i)
                for (int j = 0; j < N; ++j) cells.push_back({i, j});
        QString line = num(r.frequency);
        int onLine = 0;
        for (const auto& [i, j] : cells) {
            if (N > 2 && onLine == 4) {
                out += line + QLatin1Char('\n');
                line.clear();
                onLine = 0;
            }
            line += QLatin1Char(' ') + num(r.S(i, j).real()) + QLatin1Char(' ') + num(r.S(i, j).imag());
            ++onLine;
            if (N > 2 && j == N - 1) {
                out += line + QLatin1Char('\n');
                line.clear();
                onLine = 0;
            }
        }
        if (!line.isEmpty()) out += line + QLatin1Char('\n');
    }
    return out;
}

// ------------------------------------------------------- the physics' node

namespace {

/// \a text a length in the geometry's unit (\a unitScale metres; a plain
/// number in it); NaN and why in \a why.
double lengthIn(const QString& text, double unitScale, const Scope& scope, QString* why)
{
    Expression::Options o;
    o.lengthUnit = unitScale;
    o.numbersAreLengths = true;
    const Expression e = Expression::compile(text, scope, o);
    if (!e.isValid() || !e.isConstant()) {
        *why = e.isValid() ? tr("not a constant") : e.error();
        return NaN;
    }
    return e.constant();
}

} // namespace

bool readEmeDevice(const Model& model, const Node& physics, const Topology3& topology, const ParameterScope& parameters, double frequency,
                   EmeDevice* device, QString* error)
{
    (void)model;
    (void)frequency;
    EmeDevice d;
    d.unitScale = topology.unitScale;
    const bool onPath = physics.text(QStringLiteral("axis")) == QLatin1String("path");
    const bool alongY = physics.text(QStringLiteral("axis")) == QLatin1String("y");
    d.along = alongY ? QPointF(0, 1) : QPointF(1, 0);
    d.across = alongY ? QPointF(-1, 0) : QPointF(0, 1);
    d.origin = QPointF(0, 0);
    const Box3& b = topology.bounds;
    QStringList problems;
    auto len = [&](const QString& text, double fallback, const QString& what) {
        if (text.trimmed().isEmpty()) return fallback;
        QString why;
        const double v = lengthIn(text, topology.unitScale, parameters.scope, &why);
        if (!why.isEmpty()) problems << tr("%1: %2: %3").arg(physics.name(), what, why);
        return v;
    };
    // (s along the axis, u across it: along +y, u is -x.)
    d.s0 = len(physics.text(QStringLiteral("from")), alongY ? b.y0 : b.x0, tr("where the device begins"));
    d.s1 = len(physics.text(QStringLiteral("to")), alongY ? b.y1 : b.x1, tr("where it ends"));
    auto uRange = [&](const QStringList& pair, double* u0, double* u1, const QString& what) {
        const double a = len(pair.value(0), NaN, what), c = len(pair.value(1), NaN, what);
        *u0 = alongY && !onPath ? -std::max(a, c) : std::min(a, c);
        *u1 = alongY && !onPath ? -std::min(a, c) : std::max(a, c);
    };
    if (onPath) {
        // Its points, its corners rounded: straight pieces and arcs.
        std::vector<QPointF> pts;
        for (const QJsonValue& v : physics.value(QStringLiteral("points")).toArray()) {
            const QJsonArray xy = v.toArray();
            pts.push_back(QPointF(len(xy.at(0).toString(), NaN, tr("a point of its path")), len(xy.at(1).toString(), NaN, tr("a point of its path"))));
        }
        const double R = len(physics.text(QStringLiteral("radius")), 0, tr("its bend radius"));
        if (pts.size() < 2) problems << tr("%1: its path wants two points at least").arg(physics.name());
        else if (!(R > 0)) problems << tr("%1: its bend radius must be more than nought").arg(physics.name());
        else {
            auto unit = [](QPointF v) {
                const double n = std::hypot(v.x(), v.y());
                return n > 0 ? v / n : QPointF();
            };
            QPointF at = pts.front();
            double before = 0;   // the last corner's tangent length along this piece
            for (std::size_t i = 1; i < pts.size(); ++i) {
                const QPointF d1 = unit(pts[i] - pts[i - 1]);
                const double piece = std::hypot(pts[i].x() - pts[i - 1].x(), pts[i].y() - pts[i - 1].y());
                double t = 0, theta = 0;
                if (i + 1 < pts.size()) {
                    const QPointF d2 = unit(pts[i + 1] - pts[i]);
                    theta = std::atan2(d1.x() * d2.y() - d1.y() * d2.x(), d1.x() * d2.x() + d1.y() * d2.y());
                    t = R * std::tan(std::abs(theta) / 2);
                }
                if (piece < before + t - 1e-9 * std::max(1.0, piece) || !(piece > 0)) {
                    problems << tr("%1: its path's piece %2 is %3 long, its bends need %4 of it: points further apart, or a smaller radius")
                                    .arg(physics.name())
                                    .arg(i)
                                    .arg(formatNumber(piece, 4), formatNumber(before + t, 4));
                    break;
                }
                const QPointF end = pts[i] - d1 * t;
                const double straight = std::hypot(end.x() - at.x(), end.y() - at.y());
                if (straight > 1e-9 * std::max(1.0, piece)) {
                    EmePiece s;
                    s.start = at;
                    s.dir = d1;
                    s.length = straight;
                    d.path.push_back(s);
                }
                if (t > 0 && std::abs(theta) > 1e-12) {
                    EmePiece a;
                    a.arc = true;
                    a.radius = R;
                    a.sweep = theta;
                    const QPointF normal = theta > 0 ? QPointF(-d1.y(), d1.x()) : QPointF(d1.y(), -d1.x());
                    a.center = end + normal * R;
                    a.angle0 = std::atan2(end.y() - a.center.y(), end.x() - a.center.x());
                    a.length = R * std::abs(theta);
                    d.path.push_back(a);
                    const QPointF d2 = unit(pts[i + 1] - pts[i]);
                    at = pts[i] + d2 * t;
                } else
                    at = end;
                before = t;
            }
            double total = 0;
            for (const EmePiece& p : d.path) total += p.length;
            d.s0 = 0;
            d.s1 = total;
        }
        if (physics.text(QStringLiteral("window")) != QLatin1String("box"))
            problems << tr("%1: a path's cross-section is a box of it (Cross-section: a box): across from its centre line, and in z").arg(physics.name());
    }
    if (physics.text(QStringLiteral("window")) == QLatin1String("box")) {
        uRange(physics.pair(QStringLiteral("across")), &d.u0, &d.u1, tr("its cross-section across"));
        const QStringList h = physics.pair(QStringLiteral("height"));
        const double z0 = len(h.value(0), NaN, tr("its cross-section's z")), z1 = len(h.value(1), NaN, tr("its cross-section's z"));
        d.z0 = std::min(z0, z1);
        d.z1 = std::max(z0, z1);
    } else {
        d.u0 = alongY ? -b.x1 : b.y0;
        d.u1 = alongY ? -b.x0 : b.y1;
        d.z0 = b.z0;
        d.z1 = b.z1;
    }
    d.settings.modes = std::clamp(physics.integer(QStringLiteral("modes")), 1, 400);
    d.settings.order = physics.text(QStringLiteral("order")) == QLatin1String("1") ? 1 : 2;
    {
        const Expression e = Expression::compile(physics.text(QStringLiteral("resolution")), parameters.scope);
        const double v = e.isValid() && e.isConstant() ? e.constant() : NaN;
        if (!(v >= 2 && v <= 200)) problems << tr("%1: elements per wavelength from 2 to 200").arg(physics.name());
        else d.settings.resolution = v;
    }
    // (Metal, magnetic or absorbing: the model offers no PML - see EmeSettings.)
    auto wall = [](const QString& w) { return w == QLatin1String("pmc") || w == QLatin1String("absorbing") ? w : QStringLiteral("pec"); };
    d.settings.sides = wall(physics.text(QStringLiteral("sides")));
    d.settings.floors = wall(physics.text(QStringLiteral("floors")));
    if (d.settings.sides == QLatin1String("absorbing") || d.settings.floors == QLatin1String("absorbing")) {
        d.settings.absorberThickness = len(physics.text(QStringLiteral("absorberthickness")), 1e-6 / topology.unitScale, tr("its absorbing layer's thickness"));
        const Expression k = Expression::compile(physics.text(QStringLiteral("absorberkappa")), parameters.scope);
        d.settings.absorberKappa = k.isValid() && k.isConstant() ? k.constant() : NaN;
        d.settings.absorberLayers = std::clamp(physics.integer(QStringLiteral("absorbersteps")), 1, 20);
        if (!(d.settings.absorberThickness > 0)) problems << tr("%1: its absorbing layer's thickness must be more than nought").arg(physics.name());
        if (!(d.settings.absorberKappa > 0)) problems << tr("%1: its absorbing layer's extinction must be more than nought").arg(physics.name());
    }
    d.maxCell = len(physics.text(QStringLiteral("cell")), 0, tr("its longest cell"));
    if (!(d.maxCell >= 0)) problems << tr("%1: its longest cell is no length").arg(physics.name());
    if (!(d.s1 > d.s0)) problems << tr("%1: the device ends where it begins, or before").arg(physics.name());
    if (!(d.u1 > d.u0) || !(d.z1 > d.z0)) problems << tr("%1: its cross-section is empty").arg(physics.name());
    QSet<QString> names;
    for (const Node& c : physics.children) {
        if (!c.enabled || c.type != QLatin1String("eme/port")) continue;
        EmePort p;
        p.name = c.text(QStringLiteral("name")).trimmed();
        if (p.name.isEmpty()) problems << tr("%1: a port without a name").arg(c.name());
        if (names.contains(p.name)) problems << tr("two ports named %1: each its own").arg(p.name);
        names.insert(p.name);
        p.atEnd = c.text(QStringLiteral("end")) == QLatin1String("end");
        p.whole = c.text(QStringLiteral("window")) != QLatin1String("box");
        if (p.whole) {
            p.u0 = d.u0;
            p.u1 = d.u1;
            p.z0 = d.z0;
            p.z1 = d.z1;
        } else {
            uRange(c.pair(QStringLiteral("across")), &p.u0, &p.u1, tr("port %1's box across").arg(p.name));
            const QStringList h = c.pair(QStringLiteral("height"));
            const double z0 = len(h.value(0), NaN, tr("port %1's z").arg(p.name)), z1 = len(h.value(1), NaN, tr("port %1's z").arg(p.name));
            p.z0 = std::min(z0, z1);
            p.z1 = std::max(z0, z1);
            p.u0 = std::max(p.u0, d.u0);
            p.u1 = std::min(p.u1, d.u1);
            p.z0 = std::max(p.z0, d.z0);
            p.z1 = std::min(p.z1, d.z1);
            if (!(p.u1 > p.u0) || !(p.z1 > p.z0)) problems << tr("port %1: its box is outside the cross-section").arg(p.name);
        }
        p.modes.clear();
        for (const QString& m : c.text(QStringLiteral("modes")).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString t = m.trimmed().toUpper();
            static const QRegularExpression re(QStringLiteral("^(TE|TM)\\d+$|^\\d+$"));
            if (!re.match(t).hasMatch()) problems << tr("port %1: a mode is TE0, TM1... or a number from 1, not %2").arg(p.name, m.trimmed());
            else p.modes << t;
        }
        if (p.modes.isEmpty()) problems << tr("port %1 takes no mode").arg(p.name);
        p.excite = c.flag(QStringLiteral("excite"));
        d.ports.push_back(p);
    }
    if (d.ports.empty()) problems << tr("%1 has no Optical Port").arg(physics.name());
    // Periodic sections: their period's ends along the axis, how many in a row.
    for (const Node& c : physics.children) {
        if (!c.enabled || c.type != QLatin1String("eme/periodic")) continue;
        EmePeriodic p;
        const double a = len(c.text(QStringLiteral("from")), NaN, tr("%1's start").arg(c.name())),
                     b = len(c.text(QStringLiteral("to")), NaN, tr("%1's end").arg(c.name()));
        p.s0 = std::min(a, b);
        p.s1 = std::max(a, b);
        const Expression e = Expression::compile(c.text(QStringLiteral("periods")), parameters.scope);
        const double n = e.isValid() && e.isConstant() ? e.constant() : NaN;
        if (!(n >= 1 && n <= 1e9) || std::abs(n - std::round(n)) > 1e-9 * n)
            problems << tr("%1: its periods a whole number from 1, not %2").arg(c.name(), c.text(QStringLiteral("periods")));
        else p.periods = std::llround(n);
        const double tol = 1e-9 * std::max({1.0, std::abs(d.s0), std::abs(d.s1)});
        if (!std::isfinite(a) || !std::isfinite(b)) problems << tr("%1: its period's start and end are lengths along the axis").arg(c.name());
        else if (!(p.s1 > p.s0)) problems << tr("%1: its period ends where it begins").arg(c.name());
        else if (p.s0 < d.s0 - tol || p.s1 > d.s1 + tol)
            problems << tr("%1: its period, %2 to %3, reaches outside the device (%4 to %5)")
                            .arg(c.name(), formatNumber(p.s0, 6), formatNumber(p.s1, 6), formatNumber(d.s0, 6), formatNumber(d.s1, 6));
        d.periodic.push_back(p);
    }
    std::sort(d.periodic.begin(), d.periodic.end(), [](const EmePeriodic& x, const EmePeriodic& y) { return x.s0 < y.s0; });
    for (std::size_t i = 1; i < d.periodic.size(); ++i)
        if (d.periodic[i].s0 < d.periodic[i - 1].s1 - 1e-9 * std::max(1.0, std::abs(d.periodic[i].s0)))
            problems << tr("%1: two periodic sections overlap (%2 to %3 and %4 to %5)")
                            .arg(physics.name(), formatNumber(d.periodic[i - 1].s0, 6), formatNumber(d.periodic[i - 1].s1, 6),
                                 formatNumber(d.periodic[i].s0, 6), formatNumber(d.periodic[i].s1, 6));
    if (!problems.isEmpty()) {
        *error = problems.join(QLatin1Char('\n'));
        return false;
    }
    *device = d;
    return true;
}

} // namespace qucs_s::fem
