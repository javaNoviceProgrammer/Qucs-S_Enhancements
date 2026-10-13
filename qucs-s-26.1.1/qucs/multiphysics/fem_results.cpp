/*
 * fem_results.cpp - what a multiphysics solution shows
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_results.h"

#include <QRegularExpression>

#include <QCoreApplication>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Results", text);
}

constexpr double C0 = 299792458.0;

/// The space whose map a point is placed by: the finest (an arc's
/// quadratic elements curved).
const Space* finest(const Solution& s)
{
    const Space* best = nullptr;
    for (const Field& f : s.fields())
        if (!best || f.space->order() > best->order()) best = f.space.get();
    return best;
}

void parallel(int n, const std::function<void(int, int)>& work)
{
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    if (n < 4000 || threads == 1) {
        work(0, n);
        return;
    }
    std::vector<std::thread> pool;
    const int chunk = (n + threads - 1) / threads;
    for (int k = 0; k < threads; ++k) {
        const int b = k * chunk, e = std::min(n, b + chunk);
        if (b < e) pool.emplace_back(work, b, e);
    }
    for (std::thread& t : pool) t.join();
}

/// (xi, eta) of \a p (the geometry's unit) in triangle \a t made exact
/// where the element is curved: its quadratic map inverted by Newton.
void refine(const Solution& s, int t, QPointF p, double& xi, double& eta)
{
    const Space* sp = finest(s);
    if (!sp || !sp->curved(t)) return;
    const QPointF target = p * s.mesh().unitScale;
    for (int it = 0; it < 8; ++it) {
        const Space::Map m = sp->map(t, xi, eta);
        const QPointF r = target - m.x;
        // (xi, eta) += J^-1 r, J^-1 = invT transposed.
        const double dxi = m.invT[0][0] * r.x() + m.invT[1][0] * r.y();
        const double deta = m.invT[0][1] * r.x() + m.invT[1][1] * r.y();
        xi += dxi;
        eta += deta;
        if (std::abs(dxi) + std::abs(deta) < 1e-14) break;
    }
    xi = std::clamp(xi, 0.0, 1.0);
    eta = std::clamp(eta, 0.0, 1.0 - xi);
}

QString shown(double si, const Dim& dim, const DisplayUnit& unit)
{
    if (unit.name.isEmpty()) return formatQuantity(si, dim);
    return formatNumber((si - unit.offset) / unit.scale, 5) + QLatin1Char(' ') + unit.name;
}

/// A quotient's unit as it is written - semi.I_k/semi.Pin in A/W, not
/// s³·A/(m²·kg) - when its dimension has no name of its own and each side's
/// has (A/W is 1/V too: the expression says which); else none.
DisplayUnit quotientUnit(const QString& text, const Dim& dim, const Scope& scope)
{
    DisplayUnit u;
    if (!unitName(dim).isEmpty() || dim.isNone()) return u;
    // One '/' outside brackets, nothing else between its two sides there
    // (a sum, a product, a power: not a plain quotient).
    int depth = 0, at = -1;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c == QLatin1Char('(') || c == QLatin1Char('[')) ++depth;
        else if (c == QLatin1Char(')') || c == QLatin1Char(']')) --depth;
        else if (depth > 0) continue;
        else if (c == QLatin1Char('/')) {
            if (at >= 0) return u;
            at = i;
        } else if (c == QLatin1Char('+') || c == QLatin1Char('*') || c == QLatin1Char('^')) {
            // (An exponent's sign: 1e+3.)
            if (!(c == QLatin1Char('+') && i > 1 && (text[i - 1] == QLatin1Char('e') || text[i - 1] == QLatin1Char('E')) && text[i - 2].isDigit()))
                return u;
        } else if (c == QLatin1Char('-') && i > 0
                   && !(i > 1 && (text[i - 1] == QLatin1Char('e') || text[i - 1] == QLatin1Char('E')) && text[i - 2].isDigit())) {
            return u;
        }
    }
    if (at <= 0) return u;
    const Expression top = Expression::compile(text.left(at), scope), bottom = Expression::compile(text.mid(at + 1), scope);
    if (!top.isValid() || !bottom.isValid() || !(top.dim() / bottom.dim() == dim)) return u;
    const QString a = unitName(top.dim()), b = unitName(bottom.dim());
    if (a.isEmpty() || b.isEmpty()) return u;
    u.name = a + QLatin1Char('/') + (b.contains(QLatin1Char('/')) || b.contains(QChar(0x00B7)) ? QLatin1Char('(') + b + QLatin1Char(')') : b);
    return u;
}

} // namespace

// ------------------------------------------------------------------ Locator

Locator::Locator(const Mesh& mesh) : a_mesh(mesh)
{
    a_box = mesh.bounds();
    const int nt = int(mesh.triangles.size());
    const int n = std::clamp(int(std::sqrt(double(std::max(nt, 1)))), 1, 1024);
    const double aspect = a_box.height() > 0 ? a_box.width() / a_box.height() : 1;
    a_nx = std::clamp(int(n * std::sqrt(aspect)), 1, 2048);
    a_ny = std::clamp(int(n / std::sqrt(aspect)), 1, 2048);
    a_cw = std::max(a_box.width() / a_nx, 1e-300);
    a_ch = std::max(a_box.height() / a_ny, 1e-300);
    a_cells.resize(std::size_t(a_nx) * std::size_t(a_ny));
    for (int t = 0; t < nt; ++t) {
        const auto& v = mesh.triangles[std::size_t(t)];
        double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        for (int k : v) {
            const QPointF p = mesh.nodes[std::size_t(k)];
            x0 = std::min(x0, p.x());
            y0 = std::min(y0, p.y());
            x1 = std::max(x1, p.x());
            y1 = std::max(y1, p.y());
        }
        const int i0 = std::clamp(int((x0 - a_box.left()) / a_cw), 0, a_nx - 1), i1 = std::clamp(int((x1 - a_box.left()) / a_cw), 0, a_nx - 1);
        const int j0 = std::clamp(int((y0 - a_box.top()) / a_ch), 0, a_ny - 1), j1 = std::clamp(int((y1 - a_box.top()) / a_ch), 0, a_ny - 1);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) a_cells[std::size_t(j) * std::size_t(a_nx) + std::size_t(i)].push_back(t);
    }
}

bool Locator::locate(QPointF p, int* t, double* xi, double* eta) const
{
    const double tol = 1e-9 * std::max(a_box.width(), a_box.height());
    if (!a_box.adjusted(-tol, -tol, tol, tol).contains(p)) return false;
    const int i = std::clamp(int((p.x() - a_box.left()) / a_cw), 0, a_nx - 1);
    const int j = std::clamp(int((p.y() - a_box.top()) / a_ch), 0, a_ny - 1);
    int best = -1;
    double bestWorst = -INFINITY, bx = 0, by = 0;
    for (int k : a_cells[std::size_t(j) * std::size_t(a_nx) + std::size_t(i)]) {
        const auto& v = a_mesh.triangles[std::size_t(k)];
        const QPointF p0 = a_mesh.nodes[std::size_t(v[0])], p1 = a_mesh.nodes[std::size_t(v[1])], p2 = a_mesh.nodes[std::size_t(v[2])];
        const double ax = p1.x() - p0.x(), bxx = p2.x() - p0.x(), ay = p1.y() - p0.y(), byy = p2.y() - p0.y();
        const double det = ax * byy - bxx * ay;
        if (det == 0) continue;
        const double qx = p.x() - p0.x(), qy = p.y() - p0.y();
        const double u = (qx * byy - bxx * qy) / det, w = (ax * qy - qx * ay) / det;
        const double worst = std::min({u, w, 1 - u - w});
        if (worst >= 0) {
            *t = k;
            *xi = u;
            *eta = w;
            return true;
        }
        // (on an edge, within rounding)
        if (worst > bestWorst) {
            bestWorst = worst;
            best = k;
            bx = u;
            by = w;
        }
    }
    if (best >= 0 && bestWorst > -1e-9) {
        *t = best;
        *xi = std::clamp(bx, 0.0, 1.0);
        *eta = std::clamp(by, 0.0, 1.0 - *xi);
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ Units

DisplayUnit displayUnit(const QString& text, const Dim& dim, QString* warning)
{
    DisplayUnit u;
    if (text.trimmed().isEmpty()) return u;
    QString why;
    const std::optional<Unit> parsed = parseUnit(text.trimmed(), &why);
    if (!parsed) {
        if (warning) *warning = why;
        return u;
    }
    if (!(parsed->dim == dim)) {
        if (warning) *warning = tr("%1 is not a unit of %2").arg(text.trimmed(), dimName(dim));
        return u;
    }
    u.scale = parsed->scale;
    u.offset = parsed->offset;
    u.name = text.trimmed();
    return u;
}

DisplayUnit displayUnitOf(const Solution& solution, const QString& expression, const QString& text, const Dim& dim, QString* warning)
{
    for (const Variable& g : solution.globals()) {
        if (g.name != expression.trimmed() || g.unit.isEmpty()) continue;
        const QString asked = text.trimmed();
        if (asked.isEmpty()) {
            DisplayUnit own;
            own.name = g.unit;
            return own;
        }
        // dB/m asked as dB/cm: 1/cm of 1/m's dimension, the dB kept.
        const QString word = g.unit.section(QLatin1Char('/'), 0, 0);
        if (asked.startsWith(word + QLatin1Char('/'))) {
            DisplayUnit per = displayUnit(QStringLiteral("1/") + asked.mid(word.size() + 1), dim);
            if (!per.name.isEmpty()) {
                per.name = asked;
                return per;
            }
        }
        // A unit of its dimension (1/cm: the number per cm); else its own,
        // and why - not its number under the dimension's SI unit.
        QString why;
        const DisplayUnit plain = displayUnit(asked, dim, &why);
        if (!plain.name.isEmpty()) return plain;
        if (warning) *warning = tr("%1 is not a unit of %2 (%3)").arg(asked, g.name, g.unit);
        DisplayUnit own;
        own.name = g.unit;
        return own;
    }
    return displayUnit(text, dim, warning);
}

// ------------------------------------------------------------------ Surfaces

SurfaceData surfaceData(const Solution& solution, const QString& expression, const QString& unit, Deformation* deformation)
{
    SurfaceData out;
    const Expression e = Expression::compile(expression, solution.scope());
    if (!e.isValid()) {
        out.error = e.error();
        return out;
    }
    Expression dx, dy;
    if (deformation) {
        dx = Expression::compile(deformation->x, solution.scope());
        dy = Expression::compile(deformation->y, solution.scope());
        if (!dx.isValid() || !dy.isValid()) {
            out.error = tr("the deformation: %1").arg(dx.isValid() ? dy.error() : dx.error());
            return out;
        }
    }
    std::vector<QPointF> moved;   // each point's displacement (m)
    out.dim = e.dim();
    out.unit = displayUnit(unit, e.dim(), &out.warning);
    if (!e.warnings().isEmpty()) out.warning += (out.warning.isEmpty() ? QString() : QStringLiteral("; ")) + e.warnings().join(QLatin1String("; "));
    const Mesh& mesh = solution.mesh();
    const Space* sp = finest(solution);
    const bool split = sp && sp->order() == 2;
    // A triangle's points: its corners, or its corners and edges' middles
    // in four triangles.
    static const double ref[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0.5, 0}, {0.5, 0.5}, {0, 0.5}};
    static const int four[4][3] = {{0, 3, 5}, {3, 1, 4}, {5, 4, 2}, {3, 4, 5}};
    const int nt = int(mesh.triangles.size());
    const int perTri = split ? 12 : 3;
    out.points.resize(std::size_t(nt) * std::size_t(perTri));
    out.values.resize(out.points.size());
    if (deformation) moved.assign(out.points.size(), QPointF());
    const double s = mesh.unitScale;
    parallel(nt, [&](int begin, int end) {
        PointEvaluator ev(solution);
        QPointF pts[6], disp[6];
        double vals[6];
        for (int t = begin; t < end; ++t) {
            const int n = split ? 6 : 3;
            for (int k = 0; k < n; ++k) {
                ev.moveTo(t, ref[k][0], ref[k][1]);
                if (sp) pts[k] = sp->map(t, ref[k][0], ref[k][1]).x / s;
                else pts[k] = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])];
                vals[k] = (ev.eval(e) - out.unit.offset) / out.unit.scale;
                if (deformation) {
                    disp[k] = QPointF(ev.eval(dx), ev.eval(dy));
                    if (!std::isfinite(disp[k].x()) || !std::isfinite(disp[k].y())) disp[k] = QPointF();
                }
            }
            const std::size_t base = std::size_t(t) * std::size_t(perTri);
            if (!split) {
                for (int k = 0; k < 3; ++k) {
                    out.points[base + std::size_t(k)] = pts[k];
                    out.values[base + std::size_t(k)] = vals[k];
                    if (deformation) moved[base + std::size_t(k)] = disp[k];
                }
            } else {
                for (int q = 0; q < 4; ++q)
                    for (int k = 0; k < 3; ++k) {
                        out.points[base + std::size_t(q * 3 + k)] = pts[four[q][k]];
                        out.values[base + std::size_t(q * 3 + k)] = vals[four[q][k]];
                        if (deformation) moved[base + std::size_t(q * 3 + k)] = disp[four[q][k]];
                    }
            }
        }
    });
    if (deformation) {
        // Moved: the largest displacement a tenth of the model, or as given.
        double largest = 0;
        for (QPointF d : moved) largest = std::max(largest, std::hypot(d.x(), d.y()));
        const QRectF b = mesh.bounds();
        const double size = std::max(b.width(), b.height()) * s;
        double scale = deformation->scale;
        if (!(scale > 0)) scale = largest > 0 ? 0.1 * size / largest : 1;
        deformation->used = scale;
        deformation->largest = largest;
        for (std::size_t i = 0; i < moved.size(); ++i) out.points[i] += moved[i] * (scale / s);
    }
    double lo = INFINITY, hi = -INFINITY;
    for (double v : out.values)
        if (std::isfinite(v)) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    if (lo > hi) {
        out.error = tr("%1 is defined nowhere: is the study solved, the expression of its physics?").arg(expression);
        lo = hi = 0;
    }
    out.min = lo;
    out.max = hi;
    return out;
}

ContourData contourData(const SurfaceData& surface, int levels)
{
    ContourData out;
    levels = std::clamp(levels, 1, 500);
    const double lo = surface.min, hi = surface.max;
    if (!(hi > lo)) return out;
    for (int k = 0; k < levels; ++k) out.levels.push_back(lo + (k + 0.5) * (hi - lo) / levels);
    out.lines.resize(out.levels.size());
    const std::size_t n = surface.points.size() / 3;
    for (std::size_t t = 0; t < n; ++t) {
        const QPointF* p = &surface.points[t * 3];
        const double* v = &surface.values[t * 3];
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
        const double tmin = std::min({v[0], v[1], v[2]}), tmax = std::max({v[0], v[1], v[2]});
        for (std::size_t l = 0; l < out.levels.size(); ++l) {
            const double L = out.levels[l];
            if (L < tmin || L > tmax) continue;
            QPointF cut[3];
            int found = 0;
            for (int k = 0; k < 3 && found < 2; ++k) {
                const double a = v[k], b = v[(k + 1) % 3];
                if ((a < L && b >= L) || (a >= L && b < L)) {
                    const double u = (L - a) / (b - a);
                    cut[found++] = p[k] + (p[(k + 1) % 3] - p[k]) * u;
                }
            }
            if (found == 2) out.lines[l].emplace_back(cut[0], cut[1]);
        }
    }
    return out;
}

ArrowData arrowData(const Solution& solution, const QString& x, const QString& y, const QRectF& region, int nx, int ny)
{
    ArrowData out;
    const Expression ex = Expression::compile(x, solution.scope());
    const Expression ey = Expression::compile(y, solution.scope());
    if (!ex.isValid() || !ey.isValid()) {
        out.error = !ex.isValid() ? ex.error() : ey.error();
        return out;
    }
    nx = std::clamp(nx, 1, 400);
    ny = std::clamp(ny, 1, 400);
    const Locator locator(solution.mesh());
    PointEvaluator ev(solution);
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const QPointF p(region.left() + region.width() * (i + 0.5) / nx, region.top() + region.height() * (j + 0.5) / ny);
            int t = -1;
            double xi = 0, eta = 0;
            if (!locator.locate(p, &t, &xi, &eta)) continue;
            refine(solution, t, p, xi, eta);
            ev.moveTo(t, xi, eta);
            const QPointF v(ev.eval(ex), ev.eval(ey));
            if (!std::isfinite(v.x()) || !std::isfinite(v.y())) continue;
            out.at.push_back(p);
            out.vector.push_back(v);
            out.longest = std::max(out.longest, std::hypot(v.x(), v.y()));
        }
    return out;
}

LineData lineData(const Solution& solution, const Locator& locator, const QString& expression, QPointF a, QPointF b, int count)
{
    LineData out;
    const Expression e = Expression::compile(expression, solution.scope());
    if (!e.isValid()) {
        out.error = e.error();
        return out;
    }
    out.dim = e.dim();
    count = std::clamp(count, 2, 100000);
    const double s = solution.mesh().unitScale;
    const double length = std::hypot(b.x() - a.x(), b.y() - a.y()) * s;
    PointEvaluator ev(solution);
    for (int i = 0; i < count; ++i) {
        const double u = double(i) / (count - 1);
        const QPointF p = a + (b - a) * u;
        out.points.push_back(p);
        out.along.push_back(u * length);
        int t = -1;
        double xi = 0, eta = 0;
        double v = std::numeric_limits<double>::quiet_NaN();
        if (e.isConstant()) v = e.constant();
        else if (locator.locate(p, &t, &xi, &eta)) {
            refine(solution, t, p, xi, eta);
            ev.moveTo(t, xi, eta);
            v = ev.eval(e);
        }
        out.values.push_back(std::isfinite(v) ? v : std::numeric_limits<double>::quiet_NaN());
    }
    return out;
}

std::optional<double> evaluateAt(const Solution& solution, const Locator& locator, const QString& expression, QPointF p,
                                 QString* error, Dim* dim)
{
    const Expression e = Expression::compile(expression, solution.scope());
    if (!e.isValid()) {
        if (error) *error = e.error();
        return std::nullopt;
    }
    if (dim) *dim = e.dim();
    if (e.isConstant()) return e.constant();
    int t = -1;
    double xi = 0, eta = 0;
    if (!locator.locate(p, &t, &xi, &eta)) {
        if (error) *error = tr("(%1, %2) is in no domain").arg(p.x()).arg(p.y());
        return std::nullopt;
    }
    refine(solution, t, p, xi, eta);
    PointEvaluator ev(solution);
    ev.moveTo(t, xi, eta);
    const double v = ev.eval(e);
    if (!std::isfinite(v)) {
        if (error) *error = tr("%1 is not defined at (%2, %3)").arg(expression).arg(p.x()).arg(p.y());
        return std::nullopt;
    }
    return v;
}

// ------------------------------------------------------------------ Derived values

namespace {

struct Over {
    double integral = 0, measure = 0, max = -INFINITY, min = INFINITY;
    bool any = false;
};

Over overDomains(const Solution& s, const Expression& e, const QVector<int>& domains, bool revolved)
{
    Over o;
    const Mesh& mesh = s.mesh();
    const Space* sp = finest(s);
    QSet<int> set(domains.begin(), domains.end());
    PointEvaluator ev(s);
    static const double corners[3][2] = {{0, 0}, {1, 0}, {0, 1}};
    for (int t = 0; t < int(mesh.triangles.size()); ++t) {
        if (!set.contains(mesh.domain[std::size_t(t)])) continue;
        const TriangleRule& rule = triangleRule(sp && sp->curved(t) ? 5 : 4);
        for (const auto& q : rule.points) {
            ev.moveTo(t, q[0], q[1]);
            const double v = ev.eval(e);
            if (!std::isfinite(v)) continue;
            const Space::Map m = sp ? sp->map(t, q[0], q[1]) : Space::Map();
            const double det = std::abs(m.det) * (revolved ? 2 * M_PI * m.x.x() : 1);
            o.integral += q[2] * det * v;
            o.measure += q[2] * det;
            o.max = std::max(o.max, v);
            o.min = std::min(o.min, v);
            o.any = true;
        }
        for (const auto& c : corners) {
            ev.moveTo(t, c[0], c[1]);
            const double v = ev.eval(e);
            if (!std::isfinite(v)) continue;
            o.max = std::max(o.max, v);
            o.min = std::min(o.min, v);
        }
    }
    return o;
}

Over overBoundaries(const Solution& s, const Expression& e, const QVector<int>& boundaries, bool revolved)
{
    Over o;
    const Mesh& mesh = s.mesh();
    const Space* sp = finest(s);
    QSet<int> set(boundaries.begin(), boundaries.end());
    PointEvaluator ev(s);
    for (int ei = 0; ei < int(mesh.edges.size()); ++ei) {
        const Mesh::Edge& me = mesh.edges[std::size_t(ei)];
        if (!set.contains(me.boundary)) continue;
        for (const auto& q : lineRule(3)) {
            double jac = 0;
            QPointF x;
            if (sp) x = sp->edgePoint(ei, q[0], &jac);
            if (revolved) jac *= 2 * M_PI * x.x();
            // Its value: the average of the two sides' where both have one.
            double sum = 0;
            int sides = 0;
            for (int tri : {me.left, me.right}) {
                if (tri < 0) continue;
                const auto& tv = mesh.triangles[std::size_t(tri)];
                const int k = Space::localEdge(tv, me.a, me.b);
                const double sl = tv[std::size_t(k)] == me.a ? q[0] : 1 - q[0];
                const double xi = k == 0 ? sl : k == 1 ? 1 - sl : 0;
                const double eta = k == 0 ? 0 : k == 1 ? sl : 1 - sl;
                ev.moveTo(tri, xi, eta);
                const double v = ev.eval(e);
                if (std::isfinite(v)) {
                    sum += v;
                    ++sides;
                }
            }
            if (sides == 0) continue;
            const double v = sum / sides;
            o.integral += q[1] * jac * v;
            o.measure += q[1] * jac;
            o.max = std::max(o.max, v);
            o.min = std::min(o.min, v);
            o.any = true;
        }
    }
    return o;
}

} // namespace

LineRLGC lineRLGC(const Solution& solution, const Node& node)
{
    LineRLGC out;
    if (solution.axisymmetric()) {
        out.error = tr("a line's parameters are per metre of a 2D cross-section in the plane: not about an axis");
        return out;
    }
    const double d = solution.thickness();
    out.frequency = solution.frequency();
    const double omega = 2 * M_PI * out.frequency;
    // C and G from electrostatics: the signal terminal at its voltage, the
    // others at none.
    const QStringList esTags = node.list(QStringLiteral("physics"));
    const QString esTag = esTags.isEmpty() ? QStringLiteral("es") : esTags.first();
    const int ei = solution.fieldIndex(esTag);
    if (ei < 0 || solution.fields()[std::size_t(ei)].type != QLatin1String("electrostatics") || !solution.fields()[std::size_t(ei)].solved) {
        out.error = tr("there is no electrostatics %1 solved (its C and G)").arg(esTag);
        return out;
    }
    const Field& es = solution.fields()[std::size_t(ei)];
    const QString terminal = node.text(QStringLiteral("terminal")).trimmed().isEmpty() ? QStringLiteral("1") : node.text(QStringLiteral("terminal")).trimmed();
    double v0 = NAN, q = NAN;
    for (const Field::Terminal& t : es.terminals) {
        if (t.name == terminal) {
            v0 = t.potential;
            q = t.charge;
        } else if (!t.floating && t.potential != 0) {
            out.error = tr("%1's terminal %2 is at %3: a line's G and C want its other terminals at 0 V").arg(esTag, t.name, formatQuantity(t.potential, Dim::voltage()));
            return out;
        }
    }
    if (!std::isfinite(v0) || v0 == 0) {
        out.error = tr("%1 has no terminal %2 at a voltage").arg(esTag, terminal);
        return out;
    }
    out.C = q / v0 / d;
    for (int i = 0; i < int(es.matrix.size()); ++i)
        if (es.terminals[std::size_t(i)].name == terminal) out.C = es.matrix[std::size_t(i)][std::size_t(i)] / d;
    {
        // G = (ω ∫ ε0 εr tan δ |E|² + ∫ σ |E|²) / V0², over the plane (per metre).
        QVector<int> domains;
        for (int k = 0; k < int(es.domains.size()); ++k)
            if (es.domains[std::size_t(k)]) domains << k;
        const QString p = esTag + QLatin1Char('.');
        // (tan δ or σ not given: that term none)
        const Expression loss = Expression::compile(QStringLiteral("(%1*eps0*%2epsr*if(%2tand > 0, %2tand, 0))*%2normE^2").arg(QString::number(omega, 'g', 17), p),
                                                    solution.scope());
        const Expression cond = Expression::compile(QStringLiteral("if(%1sigma > 0, %1sigma, 0)*%1normE^2").arg(p), solution.scope());
        if (!loss.isValid() || !cond.isValid()) {
            out.error = loss.isValid() ? cond.error() : loss.error();
            return out;
        }
        const Over a = overDomains(solution, loss, domains, false), b = overDomains(solution, cond, domains, false);
        out.G = ((a.any ? a.integral : 0.0) + (b.any ? b.integral : 0.0)) / (v0 * v0);
    }
    // R and L from the magnetic fields' coils: the signal's and the return's,
    // the loop's Z = Z_ss - Z_sr - Z_rs + Z_rr.
    const QStringList mfTags = node.list(QStringLiteral("magnetic"));
    const QString mfTag = mfTags.isEmpty() ? QStringLiteral("mf") : mfTags.first();
    const int mi = solution.fieldIndex(mfTag);
    if (mi < 0 || solution.fields()[std::size_t(mi)].type != QLatin1String("magnetic") || !solution.fields()[std::size_t(mi)].solved) {
        out.error = tr("there is no magnetic fields %1 solved (its R and L)").arg(mfTag);
        return out;
    }
    const QString s = node.text(QStringLiteral("conductor")).trimmed().isEmpty() ? QStringLiteral("1") : node.text(QStringLiteral("conductor")).trimmed();
    const QString r = node.text(QStringLiteral("return")).trimmed();
    const QString p = mfTag + QLatin1Char('.');
    auto entry = [&](const QString& letter, const QString& a, const QString& b) -> std::optional<double> {
        return solution.global(p + letter + QLatin1Char('_') + a + QLatin1Char('_') + b);
    };
    auto loop = [&](const QString& letter) -> std::optional<double> {
        const std::optional<double> ss = entry(letter, s, s);
        if (!ss) return std::nullopt;
        if (r.isEmpty()) return *ss;
        const std::optional<double> sr = entry(letter, s, r), rs = entry(letter, r, s), rr = entry(letter, r, r);
        if (!sr || !rs || !rr) return std::nullopt;
        return *ss - *sr - *rs + *rr;
    };
    std::optional<double> R = loop(QStringLiteral("R")), L = loop(QStringLiteral("L"));
    if ((!R || !L) && r.isEmpty()) {
        // No matrix (the coils not swept): the coil's own.
        R = solution.global(p + QStringLiteral("R_") + s);
        L = solution.global(p + QStringLiteral("L_") + s);
    }
    if (!R || !L) {
        out.error = r.isEmpty() ? tr("%1 has no coil %2 (or its R and L): give the signal conductor a Coil, and keep the study's coil sweep on").arg(mfTag, s)
                                : tr("%1 has no impedance matrix of coils %2 and %3: keep the study's coil sweep on").arg(mfTag, s, r);
        return out;
    }
    out.R = *R / d;
    out.L = *L / d;
    // γ = √((R + jωL)(G + jωC)), Zc = √((R + jωL)/(G + jωC)).
    const std::complex<double> z(out.R, omega * out.L), y(out.G, omega * out.C);
    if (omega > 0) {
        out.gamma = std::sqrt(z * y);
        out.Zc = std::sqrt(z / y);
    } else {
        out.gamma = 0;
        out.Zc = std::sqrt(out.L / out.C);
    }
    if (out.gamma.real() < 0) out.gamma = -out.gamma;
    out.alphaDb = 20 / std::log(10.0) * out.gamma.real();
    out.velocity = omega > 0 && out.gamma.imag() > 0 ? omega / out.gamma.imag() : 1 / std::sqrt(out.L * out.C);
    out.epsEff = std::pow(299792458.0 / out.velocity, 2);
    return out;
}

NamedExpression namedExpression(const QString& item)
{
    const QString text = item.trimmed();
    // The first '=' that is no comparison's, a name before it (<=, >=, != have
    // none: a name is a word).
    for (int i = 0; i < text.size(); ++i) {
        if (text[i] != QLatin1Char('=')) continue;
        if (i + 1 < text.size() && text[i + 1] == QLatin1Char('=')) {
            ++i;
            continue;
        }
        static const QRegularExpression word(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
        const QString name = text.left(i).trimmed();
        if (!word.match(name).hasMatch()) break;
        return {name, text.mid(i + 1).trimmed()};
    }
    return {text, text};
}

DerivedResult evaluateDerived(const Solution& solution, const Node& node)
{
    DerivedResult out;
    const QString type = node.type;
    const QString unitText = node.text(QStringLiteral("unit"));
    auto add = [&](const QString& name, double value, const Dim& dim, bool useUnit = true) {
        QString warning;
        const DisplayUnit unit = useUnit ? displayUnit(unitText, dim, &warning) : DisplayUnit();
        out.values << DerivedValue{name, value, dim, shown(value, dim, unit)};
        if (!warning.isEmpty() && out.error.isEmpty()) out.error = warning;
    };
    if (type == QLatin1String("global")) {
        const QStringList exprs = node.text(QStringLiteral("expression")).split(QLatin1Char(';'), Qt::SkipEmptyParts);
        QStringList problems;
        for (const QString& raw : exprs) {
            const NamedExpression item = namedExpression(raw);
            const QString text = item.expression;
            if (text.isEmpty()) continue;
            const Expression e = Expression::compile(text, solution.scope());
            if (!e.isValid()) {
                problems << tr("%1: %2").arg(text, e.error());
                continue;
            }
            if (!e.isConstant()) {
                problems << tr("%1 varies over the model: evaluate it at a point, or integrate it").arg(text);
                continue;
            }
            // (The unit asked is of one expression alone. A global of a unit
            // its dimension cannot say - emw.loss, dB/m - in it, or over
            // another length: dB/cm.)
            QString warning;
            DisplayUnit du = displayUnitOf(solution, text, exprs.size() == 1 ? unitText : QString(), e.dim(), &warning);
            if (du.name.isEmpty()) du = quotientUnit(text, e.dim(), solution.scope());
            out.values << DerivedValue{item.name, e.constant(), e.dim(), shown(e.constant(), e.dim(), du)};
            if (!warning.isEmpty() && out.error.isEmpty()) out.error = warning;
        }
        if (!problems.isEmpty()) out.error = problems.join(QLatin1String("; "));
        return out;
    }
    if (type == QLatin1String("integral") || type == QLatin1String("average") || type == QLatin1String("maximum")
        || type == QLatin1String("minimum")) {
        const QString text = node.text(QStringLiteral("expression"));
        const Expression e = Expression::compile(text, solution.scope());
        if (!e.isValid()) {
            out.error = tr("%1: %2").arg(text, e.error());
            return out;
        }
        const Level level = levelOf(node.text(QStringLiteral("level")));
        QString why;
        const QVector<int> picked = resolveSelection(solution.topology(), solution.model(), level, node.selection(), &why);
        if (!why.isEmpty()) {
            out.error = why;
            return out;
        }
        const bool revolved = solution.axisymmetric() && node.flag(QStringLiteral("revolved"));
        const Over o = level == Level::Boundary ? overBoundaries(solution, e, picked, revolved) : overDomains(solution, e, picked, revolved);
        if (!o.any) {
            out.error = tr("%1 is defined on none of them").arg(text);
            return out;
        }
        const Dim measure = level == Level::Boundary ? (revolved ? Dim::area() : Dim::length()) : (revolved ? Dim::of(3) : Dim::area());
        // A number integrated - a function of the model's without a unit:
        // the integral's unit is the area's (length's) alone; said.
        if (type == QLatin1String("integral") && e.dim().isNone())
            if (const Node* defs = solution.model().root.child(QStringLiteral("definitions")))
                for (const Node& fn : defs->children) {
                    const QString name = fn.text(QStringLiteral("name")).trimmed();
                    if ((fn.type != QLatin1String("analytic") && fn.type != QLatin1String("interpolation")) || name.isEmpty()
                        || !fn.text(QStringLiteral("unit")).trimmed().isEmpty())
                        continue;
                    if (QRegularExpression(QStringLiteral("\\b%1\\s*\\(").arg(QRegularExpression::escape(name))).match(text).hasMatch()) {
                        out.note = tr("%1 has no unit (its Function's unit): what is integrated is a number, the integral in %2 - the "
                                      "%3's unit alone")
                                       .arg(name, dimName(measure), level == Level::Boundary ? tr("length") : tr("area"));
                        break;
                    }
                }
        if (type == QLatin1String("integral")) add(text, o.integral, e.dim() * measure);
        else if (type == QLatin1String("average")) add(text, o.integral / o.measure, e.dim());
        else if (type == QLatin1String("maximum")) add(text, o.max, e.dim());
        else add(text, o.min, e.dim());
        return out;
    }
    if (type == QLatin1String("pointeval")) {
        const QString text = node.text(QStringLiteral("expression"));
        const Locator locator(solution.mesh());
        std::vector<QPointF> where;
        if (node.text(QStringLiteral("where")) == QLatin1String("points")) {
            QString why;
            for (int p : resolveSelection(solution.topology(), solution.model(), Level::Point, node.selection(), &why))
                where.push_back(solution.topology().vertices[std::size_t(solution.topology().points[std::size_t(p)].vertex)]);
            if (!why.isEmpty()) out.error = why;
        } else {
            const double scale = solution.topology().unitScale;
            for (const QJsonValue& v : node.value(QStringLiteral("coordinates")).toArray()) {
                const QJsonArray pair = v.toArray();
                QPointF p;
                bool ok = true;
                for (int k = 0; k < 2; ++k) {
                    const QString c = pair.at(k).isDouble() ? formatNumber(pair.at(k).toDouble(), 15) : pair.at(k).toString();
                    Expression::Options options;
                    options.lengthUnit = scale;
                    options.numbersAreLengths = true;
                    const Expression ce = Expression::compile(c, solution.parameters().scope, options);
                    if (!ce.isValid() || !ce.isConstant()) {
                        out.error = tr("the coordinate %1: %2").arg(c, ce.isValid() ? tr("not a constant") : ce.error());
                        ok = false;
                        break;
                    }
                    const double value = ce.constant();
                    (k == 0 ? p.rx() : p.ry()) = value;
                }
                if (ok) where.push_back(p);
            }
        }
        for (QPointF p : where) {
            QString why;
            Dim dim;
            const std::optional<double> v = evaluateAt(solution, locator, text, p, &why, &dim);
            const QString name = tr("%1 at (%2, %3)").arg(text, formatNumber(p.x(), 6), formatNumber(p.y(), 6));
            if (!v) {
                if (out.error.isEmpty()) out.error = why;
                continue;
            }
            add(name, *v, dim);
        }
        return out;
    }
    if (type == QLatin1String("confinement")) {
        // Γ: of a mode's |E|² (or Sz, or (c ε0/2) n |E|² over Sz) in the
        // domains chosen, over the physics' all.
        const QStringList tags = node.list(QStringLiteral("physics"));
        const QString tag = tags.isEmpty() ? QStringLiteral("emw") : tags.first();
        const int fi = solution.fieldIndex(tag);
        if (fi < 0 || !solution.modeAnalysis) {
            out.error = tr("there is no mode analysis %1 solved").arg(tag);
            return out;
        }
        QString why;
        const QVector<int> picked = resolveSelection(solution.topology(), solution.model(), Level::Domain, node.selection(), &why);
        if (!why.isEmpty()) {
            out.error = why;
            return out;
        }
        QVector<int> every;
        for (int d = 0; d < int(solution.topology().domains.size()); ++d) every << d;
        const QString kind = node.text(QStringLiteral("kind"));
        const QString e2 = tag + QStringLiteral(".normE^2"), sz = tag + QStringLiteral(".Sz");
        const QString above = kind == QLatin1String("power") ? sz : kind == QLatin1String("index") ? tag + QStringLiteral(".n*") + e2 : e2;
        const QString below = kind == QLatin1String("e2") || kind.isEmpty() ? e2 : sz;
        const Expression ea = Expression::compile(above, solution.scope()), eb = Expression::compile(below, solution.scope());
        if (!ea.isValid() || !eb.isValid()) {
            out.error = ea.isValid() ? eb.error() : ea.error();
            return out;
        }
        const Over there = overDomains(solution, ea, picked, false), all = overDomains(solution, eb, every, false);
        if (!there.any || !all.any || !(std::abs(all.integral) > 0)) {
            out.error = tr("%1's mode is not defined in them").arg(tag);
            return out;
        }
        constexpr double c0eps0 = 299792458.0 * 8.8541878188e-12;
        const double gamma = kind == QLatin1String("index") ? 0.5 * c0eps0 * there.integral / all.integral : there.integral / all.integral;
        add(kind == QLatin1String("power") ? QStringLiteral("Gamma_power") : kind == QLatin1String("index") ? QStringLiteral("Gamma_index") : QStringLiteral("Gamma"),
            gamma, Dim(), false);
        return out;
    }
    if (type == QLatin1String("lineparams")) {
        if (solution.axisymmetric()) {
            out.error = tr("a line's parameters are per metre of a 2D cross-section in the plane: not about an axis");
            return out;
        }
        QStringList tags = node.list(QStringLiteral("physics"));
        QString tag = tags.isEmpty() ? QStringLiteral("es") : tags.first();
        const int fi = solution.fieldIndex(tag);
        if (fi < 0 || solution.fields()[std::size_t(fi)].type != QLatin1String("electrostatics")) {
            out.error = tr("there is no electrostatics %1 solved").arg(tag);
            return out;
        }
        if (node.text(QStringLiteral("method")) == QLatin1String("rlgc")) {
            const LineRLGC l = lineRLGC(solution, node);
            if (!l.error.isEmpty()) {
                out.error = l.error;
                return out;
            }
            const Dim perMetre = Dim::of(-1);
            add(QStringLiteral("R"), l.R, Dim::resistance() * perMetre, false);
            add(QStringLiteral("L"), l.L, Dim::of(2, 1, -2, -2) * perMetre, false);
            add(QStringLiteral("G"), l.G, Dim::conductance() * perMetre, false);
            add(QStringLiteral("C"), l.C, Dim::capacitance() * perMetre, false);
            add(QStringLiteral("Z0"), l.Zc.real(), Dim::resistance(), false);
            add(QStringLiteral("Z0_im"), l.Zc.imag(), Dim::resistance(), false);
            add(QStringLiteral("alpha"), l.alphaDb, Dim::of(-1), false);
            add(QStringLiteral("beta"), l.gamma.imag(), Dim::of(-1), false);
            add(QStringLiteral("epsilon_eff"), l.epsEff, Dim(), false);
            add(QStringLiteral("v"), l.velocity, Dim::of(1, 0, -1), false);
            out.values[6].text = formatNumber(l.alphaDb, 4) + QStringLiteral(" dB/m");
            out.values[7].text = formatNumber(l.gamma.imag(), 4) + QStringLiteral(" rad/m");
            return out;
        }
        const Field& f = solution.fields()[std::size_t(fi)];
        const QString terminal = node.text(QStringLiteral("terminal")).trimmed().isEmpty() ? QStringLiteral("1") : node.text(QStringLiteral("terminal")).trimmed();
        int index = -1;
        for (int i = 0; i < int(f.matrix.size()); ++i)
            if (f.terminals[std::size_t(i)].name == terminal) index = i;
        if (index < 0) {
            out.error = f.matrix.empty() ? tr("%1 has no capacitance matrix: give it terminals, and keep the study's terminal sweep on").arg(tag)
                                         : tr("%1 has no terminal %2").arg(tag, terminal);
            return out;
        }
        const double d = solution.thickness();
        const double c = f.matrix[std::size_t(index)][std::size_t(index)] / d;
        QString why;
        const double c0 = vacuumCapacitance(solution, tag, terminal, &why) / d;
        if (!std::isfinite(c0) || c0 <= 0) {
            out.error = why.isEmpty() ? tr("the line's capacitance in vacuum could not be found") : why;
            return out;
        }
        const Dim perMetre = Dim::of(-1);
        add(QStringLiteral("C"), c, Dim::capacitance() * perMetre, false);
        add(QStringLiteral("C0"), c0, Dim::capacitance() * perMetre, false);
        add(QStringLiteral("epsilon_eff"), c / c0, Dim(), false);
        add(QStringLiteral("Z0"), 1.0 / (C0 * std::sqrt(c * c0)), Dim::resistance(), false);
        add(QStringLiteral("L"), 1.0 / (C0 * C0 * c0), Dim::of(2, 1, -2, -2) * perMetre, false);
        add(QStringLiteral("v"), C0 / std::sqrt(c / c0), Dim::of(1, 0, -1), false);
        return out;
    }
    out.error = tr("%1 is not a derived value").arg(node.name());
    return out;
}

std::vector<Instance> instances(const SolutionSet& set, bool all, int point, const QString& time, const QString& frequency, int mode)
{
    std::vector<Instance> out;
    const int n = int(set.solutions.size());
    if (n == 0) return out;
    const int chosen = point <= 0 || point > n ? n - 1 : point - 1;
    for (int p = 0; p < n; ++p) {
        if (!all && p != chosen) continue;
        const Solution& s = *set.solutions[std::size_t(p)];
        const int snaps = s.snapshotCount();
        if (snaps == 0) {
            out.push_back({p, -1, s.time(), 0, s.label});
            continue;
        }
        int pick = snaps - 1;
        const QString& at = s.frequencyDomain ? frequency : time;
        if (!all && !at.trimmed().isEmpty()) {
            QString unlike;
            std::optional<double> v = evaluateConstant(at, s.parameters().scope, s.snapshotDim(), nullptr, &unlike);
            // (A frequency as a free-space wavelength, or a photon's energy, too.)
            if (s.frequencyDomain && (!v || !unlike.isEmpty())) {
                QString notLength, notEnergy;
                const std::optional<double> lambda = evaluateConstant(at, s.parameters().scope, Dim::length(), nullptr, &notLength);
                const std::optional<double> energy = evaluateConstant(at, s.parameters().scope, Dim::of(2, 1, -2), nullptr, &notEnergy);
                if (lambda && notLength.isEmpty() && *lambda > 0) v = 299792458.0 / *lambda;
                else if (energy && notEnergy.isEmpty() && *energy > 0) v = *energy / 6.62607015e-34;
            }
            if (v) pick = s.frequencyDomain ? s.snapshotAtFrequency(*v) : s.snapshotAt(*v);
        }
        // Modes: at that frequency (the last when none is asked), the mode asked.
        bool several = false;
        if (s.modeAnalysis) {
            for (int k = 1; k < snaps; ++k) several = several || s.snapshotFrequency(k) != s.snapshotFrequency(0);
            const double f = s.snapshotFrequency(pick);
            const int wanted = std::max(1, mode);
            int first = -1;
            for (int k = 0; k < snaps; ++k)
                if (s.snapshotFrequency(k) == f) {
                    if (first < 0) first = k;
                    if (s.snapshotMode(k) == wanted) pick = k;
                }
            if (s.snapshotMode(pick) != wanted && first >= 0) pick = first;
        }
        for (int k = 0; k < snaps; ++k) {
            if (!all && k != pick) continue;
            // (All, a mode asked: that mode alone at each.)
            if (all && mode > 0 && s.modeAnalysis && s.snapshotMode(k) != mode) continue;
            QString when = s.snapshotText(k);
            const QString mode = (s.quantumStates ? QStringLiteral("state %1") : QStringLiteral("mode %1")).arg(s.snapshotMode(k));
            if (s.modeAnalysis) when = several ? when + QStringLiteral(", ") + mode : mode;
            out.push_back({p, k, s.snapshotValue(k), s.snapshotMode(k), s.label.isEmpty() ? when : s.label + QStringLiteral(", ") + when});
        }
    }
    return out;
}

Solution* select(const SolutionSet& set, const Instance& instance)
{
    if (instance.point < 0 || instance.point >= int(set.solutions.size())) return nullptr;
    Solution* s = set.solutions[std::size_t(instance.point)].get();
    if (instance.snapshot >= 0) s->select(instance.snapshot);
    return s;
}

} // namespace qucs_s::fem
