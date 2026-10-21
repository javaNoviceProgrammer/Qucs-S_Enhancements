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

#include "fem_modeprofile.h"
#include "fem_problem.h"

#include <QRegularExpression>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
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
    // A global shown in a unit of its own dimension (qw.Ee1 in eV, qw.k in
    // 1/nm) when none is asked.
    if (text.trimmed().isEmpty())
        for (const Variable& g : solution.globals())
            if (g.name == expression.trimmed() && !g.display.isEmpty()) return displayUnit(g.display, dim);
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
    // A point variable's own unit where its dimension's name would mislead
    // (an energy density's J/m³, not Pa).
    if (text.trimmed().isEmpty())
        for (const Solution::VariableDef& d : solution.definitions())
            if (d.info.name == expression.trimmed() && !d.info.unit.isEmpty()) {
                DisplayUnit own;
                own.name = d.info.unit;
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
    const std::optional<double> v = evaluateAt(solution, locator, e, p);
    if (!v && error) *error = tr("%1 is not defined at (%2, %3)").arg(expression).arg(p.x()).arg(p.y());
    return v;
}

std::optional<double> evaluateAt(const Solution& solution, const Locator& locator, const Expression& e, QPointF p)
{
    if (!e.isValid()) return std::nullopt;
    if (e.isConstant()) return e.constant();
    int t = -1;
    double xi = 0, eta = 0;
    if (!locator.locate(p, &t, &xi, &eta)) return std::nullopt;
    refine(solution, t, p, xi, eta);
    PointEvaluator ev(solution);
    ev.moveTo(t, xi, eta);
    const double v = ev.eval(e);
    if (!std::isfinite(v)) return std::nullopt;
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
        // G = (ω ∫ ε0 E·(εr tan δ) E + ∫ E·σ E) / V0², over the plane (per
        // metre) - tensors where the dielectric is anisotropic (εr tan δ
        // along its axes).
        QVector<int> domains;
        for (int k = 0; k < int(es.domains.size()); ++k)
            if (es.domains[std::size_t(k)]) domains << k;
        const QString p = esTag + QLatin1Char('.');
        auto quadratic = [&p](const QString& t) {
            return QStringLiteral("(%1xx*%2Ex^2 + (%1xy + %1yx)*%2Ex*%2Ey + %1yy*%2Ey^2)").arg(p + t, p);
        };
        // (tan δ or σ not given: that term none)
        const Expression loss =
            Expression::compile(QStringLiteral("%1*eps0*%2").arg(QString::number(omega, 'g', 17), quadratic(QStringLiteral("epsim"))), solution.scope());
        const Expression cond = Expression::compile(quadratic(QStringLiteral("sigma")), solution.scope());
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

QStringList lineTerminals(const Node& node)
{
    QStringList out;
    for (const QString& t : node.text(QStringLiteral("terminal")).split(QLatin1Char(','), Qt::SkipEmptyParts))
        if (!t.trimmed().isEmpty()) out << t.trimmed();
    if (out.isEmpty()) out << QStringLiteral("1");
    return out;
}

namespace {

using cd = std::complex<double>;

/// The square root of a propagation constant's square: α ≥ 0, a lossless
/// one's β ≥ 0.
cd forward(cd gamma2)
{
    cd g = std::sqrt(gamma2);
    if (g.real() < 0 || (g.real() == 0 && g.imag() < 0)) g = -g;
    return g;
}

} // namespace

CoupledLines coupledLines(const Solution& solution, const Node& node)
{
    CoupledLines out;
    if (solution.axisymmetric()) {
        out.error = tr("a line's parameters are per metre of a 2D cross-section in the plane: not about an axis");
        return out;
    }
    const double d = solution.thickness();
    out.rlgc = node.text(QStringLiteral("method")) == QLatin1String("rlgc");
    out.frequency = out.rlgc ? solution.frequency() : 0.0;
    const double omega = 2 * M_PI * out.frequency;
    const QStringList esTags = node.list(QStringLiteral("physics"));
    const QString esTag = esTags.isEmpty() ? QStringLiteral("es") : esTags.first();
    const int ei = solution.fieldIndex(esTag);
    if (ei < 0 || solution.fields()[std::size_t(ei)].type != QLatin1String("electrostatics") || !solution.fields()[std::size_t(ei)].solved) {
        out.error = tr("there is no electrostatics %1 solved (its C and G)").arg(esTag);
        return out;
    }
    out.names = lineTerminals(node);
    const int n = int(out.names.size());
    // C and the losses with each terminal at 1 V in turn; C0 with every εr 1.
    TerminalMatrices tm, t0;
    QString why;
    if (!terminalMatrices(solution, esTag, out.names, false, &tm, &why) || !terminalMatrices(solution, esTag, out.names, true, &t0, &why)) {
        out.error = why;
        return out;
    }
    const std::size_t N = std::size_t(n);
    auto perMetre = [d, N](const std::vector<std::vector<double>>& m, double scale) {
        CoupledLines::Matrix r(N, std::vector<double>(N, 0));
        for (std::size_t i = 0; i < N; ++i)
            for (std::size_t j = 0; j < N; ++j) r[i][j] = scale * 0.5 * (m[i][j] + m[j][i]) / d;
        return r;
    };
    out.C = perMetre(tm.C, 1);
    out.C0 = perMetre(t0.C, 1);
    Eigen::MatrixXd C(n, n), C0m(n, n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            C(i, j) = out.C[std::size_t(i)][std::size_t(j)];
            C0m(i, j) = out.C0[std::size_t(i)][std::size_t(j)];
        }
    Eigen::MatrixXcd Z(n, n), Y(n, n);
    double w = omega;
    if (out.rlgc) {
        if (!(omega > 0)) {
            out.error = tr("coupled lines' modes want a frequency: solve the magnetic fields in the frequency domain");
            return out;
        }
        out.G = perMetre(tm.Gd, omega);
        const CoupledLines::Matrix gs = perMetre(tm.Gs, 1);
        for (std::size_t i = 0; i < N; ++i)
            for (std::size_t j = 0; j < N; ++j) out.G[i][j] += gs[i][j];
        // R and L of the coils, each conductor's (by name, as the terminals),
        // reduced against the return: Z_ij - Z_ir - Z_rj + Z_rr.
        const QStringList mfTags = node.list(QStringLiteral("magnetic"));
        const QString mfTag = mfTags.isEmpty() ? QStringLiteral("mf") : mfTags.first();
        const int mi = solution.fieldIndex(mfTag);
        if (mi < 0 || solution.fields()[std::size_t(mi)].type != QLatin1String("magnetic") || !solution.fields()[std::size_t(mi)].solved) {
            out.error = tr("there is no magnetic fields %1 solved (its R and L)").arg(mfTag);
            return out;
        }
        QStringList coils;
        for (const QString& c : node.text(QStringLiteral("conductor")).split(QLatin1Char(','), Qt::SkipEmptyParts))
            if (!c.trimmed().isEmpty()) coils << c.trimmed();
        if (coils.size() != n) {
            out.error = tr("%1 terminals (%2) want as many coils, in their order (Signal conductor: %3)")
                            .arg(n)
                            .arg(out.names.join(QStringLiteral(", ")), coils.isEmpty() ? tr("none") : coils.join(QStringLiteral(", ")));
            return out;
        }
        const QString r = node.text(QStringLiteral("return")).trimmed();
        const QString p = mfTag + QLatin1Char('.');
        auto entry = [&](const QString& a, const QString& b, cd* z) {
            const std::optional<double> re = solution.global(p + QStringLiteral("R_") + a + QLatin1Char('_') + b);
            const std::optional<double> l = solution.global(p + QStringLiteral("L_") + a + QLatin1Char('_') + b);
            if (!re || !l) return false;
            *z = cd(*re, omega * *l);
            return true;
        };
        out.R.assign(N, std::vector<double>(N, 0));
        out.L.assign(N, std::vector<double>(N, 0));
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                cd z[4];
                bool ok = entry(coils[i], coils[j], &z[0]);
                if (!r.isEmpty()) ok = ok && entry(coils[i], r, &z[1]) && entry(r, coils[j], &z[2]) && entry(r, r, &z[3]);
                if (!ok) {
                    out.error = r.isEmpty() ? tr("%1 has no impedance matrix of coils %2: keep the study's coil sweep on, each a coil fed a current")
                                                  .arg(mfTag, coils.join(QStringLiteral(", ")))
                                            : tr("%1 has no impedance matrix of coils %2 and %3: keep the study's coil sweep on, each a coil fed a current")
                                                  .arg(mfTag, coils.join(QStringLiteral(", ")), r);
                    return out;
                }
                const cd zij = r.isEmpty() ? z[0] : z[0] - z[1] - z[2] + z[3];
                out.R[std::size_t(i)][std::size_t(j)] = zij.real() / d;
                out.L[std::size_t(i)][std::size_t(j)] = zij.imag() / omega / d;
            }
        // (Symmetric: the mean of ij and ji.)
        for (std::size_t i = 0; i < N; ++i)
            for (std::size_t j = i + 1; j < N; ++j) {
                out.R[i][j] = out.R[j][i] = 0.5 * (out.R[i][j] + out.R[j][i]);
                out.L[i][j] = out.L[j][i] = 0.5 * (out.L[i][j] + out.L[j][i]);
            }
    } else {
        // Quasi-TEM: L = C0⁻¹ / c², lossless; its modes at any frequency
        // (1 GHz).
        w = 2 * M_PI * 1e9;
        const Eigen::MatrixXd Linv = C0m.inverse() / (C0 * C0);
        out.L.assign(N, std::vector<double>(N, 0));
        out.R.assign(N, std::vector<double>(N, 0));
        out.G.assign(N, std::vector<double>(N, 0));
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) out.L[std::size_t(i)][std::size_t(j)] = 0.5 * (Linv(i, j) + Linv(j, i));
    }
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            Z(i, j) = cd(out.R[std::size_t(i)][std::size_t(j)], w * out.L[std::size_t(i)][std::size_t(j)]);
            Y(i, j) = cd(out.G[std::size_t(i)][std::size_t(j)], w * out.C[std::size_t(i)][std::size_t(j)]);
        }
    const double k0 = w / C0;
    // Quasi-static modes: C v = εeff C0 v.
    Eigen::GeneralizedSelfAdjointEigenSolver<Eigen::MatrixXd> qs(C, C0m);
    if (qs.info() != Eigen::Success) {
        out.error = tr("the lines' capacitance matrix in vacuum is not positive definite: are the terminals at voltages, a ground beside them?");
        return out;
    }
    auto finish = [&N, &Y, &k0, &n, &qs](LineMode& m) {
        m.alphaDb = 20 / std::log(10.0) * m.gamma.real();
        m.nEff = m.gamma.imag() / k0;
        // i = Y v / γ; its impedance vᵀv / vᵀi.
        Eigen::VectorXcd v(n);
        for (int k = 0; k < n; ++k) v[k] = m.v[std::size_t(k)];
        const Eigen::VectorXcd iv = Y * v / m.gamma;
        m.i.assign(N, 0);
        for (int k = 0; k < n; ++k) m.i[std::size_t(k)] = iv[k];
        const cd vv = (v.transpose() * v)(0), vi = (v.transpose() * iv)(0);
        m.Z = vi != cd(0) ? vv / vi : cd(NAN, NAN);
        // Its quasi-static mode: the one most like it.
        double best = -1;
        for (int k = 0; k < n; ++k) {
            const Eigen::VectorXd q = qs.eigenvectors().col(k);
            const double like = std::abs(v.dot(q.cast<cd>())) / (v.norm() * q.norm());
            if (like > best) {
                best = like;
                m.epsEffQs = qs.eigenvalues()[k];
            }
        }
    };
    // A symmetric pair (to 1 %): its even and odd modes, of the matrices
    // made symmetric.
    auto near = [](cd a, cd b, cd scale) { return std::abs(a - b) <= 1e-2 * std::abs(scale); };
    out.symmetricPair = n == 2 && near(Z(0, 0), Z(1, 1), Z(0, 0)) && near(Y(0, 0), Y(1, 1), Y(0, 0)) && near(C(0, 0), C(1, 1), C(0, 0))
                        && near(C0m(0, 0), C0m(1, 1), C0m(0, 0));
    if (out.symmetricPair) {
        // (Made symmetric: its modes, currents and Zc of one pair.)
        Z(0, 0) = Z(1, 1) = 0.5 * (Z(0, 0) + Z(1, 1));
        Y(0, 0) = Y(1, 1) = 0.5 * (Y(0, 0) + Y(1, 1));
        for (int s = 0; s < 2; ++s) {
            const double sign = s == 0 ? 1.0 : -1.0;
            const cd z = Z(0, 0) + sign * Z(0, 1);
            const cd y = Y(0, 0) + sign * Y(0, 1);
            LineMode m;
            m.name = s == 0 ? QStringLiteral("even") : QStringLiteral("odd");
            m.gamma = forward(z * y);
            m.v = {1.0, sign};
            finish(m);
            // (Exactly the pair's: √(z/y), (C ± C12)/(C0 ± C0_12).)
            m.Z = std::sqrt(z / y);
            if (m.Z.real() < 0) m.Z = -m.Z;
            m.epsEffQs = (0.5 * (C(0, 0) + C(1, 1)) + sign * C(0, 1)) / (0.5 * (C0m(0, 0) + C0m(1, 1)) + sign * C0m(0, 1));
            out.modes.push_back(m);
        }
    } else {
        Eigen::ComplexEigenSolver<Eigen::MatrixXcd> es(Z * Y);
        if (es.info() != Eigen::Success) {
            out.error = tr("the lines' modes could not be found ((R + jωL)(G + jωC) has no eigenvectors)");
            return out;
        }
        for (int k = 0; k < n; ++k) {
            LineMode m;
            m.gamma = forward(es.eigenvalues()[k]);
            Eigen::VectorXcd v = es.eigenvectors().col(k);
            int big = 0;
            for (int j = 1; j < n; ++j)
                if (std::abs(v[j]) > std::abs(v[big])) big = j;
            v /= v[big];
            m.v.assign(N, 0);
            for (int j = 0; j < n; ++j) m.v[std::size_t(j)] = v[j];
            finish(m);
            out.modes.push_back(m);
        }
        std::sort(out.modes.begin(), out.modes.end(), [](const LineMode& a, const LineMode& b) { return a.nEff > b.nEff; });
        for (int k = 0; k < n; ++k) out.modes[std::size_t(k)].name = QString::number(k + 1);
    }
    // Zc = Tv γ⁻¹ Tv⁻¹ Z.
    {
        Eigen::MatrixXcd Tv(n, n);
        Eigen::VectorXcd g(n);
        for (int k = 0; k < n; ++k) {
            for (int j = 0; j < n; ++j) Tv(j, k) = out.modes[std::size_t(k)].v[std::size_t(j)];
            g[k] = out.modes[std::size_t(k)].gamma;
        }
        const Eigen::MatrixXcd zc = Tv * g.cwiseInverse().asDiagonal() * Tv.inverse() * Z;
        out.Zc.assign(N, std::vector<cd>(N, 0));
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) out.Zc[std::size_t(i)][std::size_t(j)] = zc(i, j);
    }
    return out;
}

QStringList expandFamilies(const QStringList& items, const Solution* solution)
{
    QStringList out;
    static const QRegularExpression family(QStringLiteral("^\\s*([A-Za-z_][A-Za-z0-9_.]*)\\*\\s*$"));
    for (const QString& item : items) {
        const QRegularExpressionMatch m = family.match(item);
        if (!m.hasMatch() || !solution) {
            out << item;
            continue;
        }
        const QString prefix = m.captured(1);
        std::vector<std::pair<qlonglong, QString>> found;
        for (const Variable& v : solution->globals()) {
            if (!v.name.startsWith(prefix) || v.name.size() == prefix.size()) continue;
            bool number = false;
            const qlonglong n = QStringView(v.name).mid(prefix.size()).toLongLong(&number);
            if (number && QStringView(v.name).mid(prefix.size()).front().isDigit()) found.emplace_back(n, v.name);
        }
        if (found.empty()) {
            out << item;
            continue;
        }
        std::sort(found.begin(), found.end());
        for (const auto& [n, name] : found) out << name;
    }
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
        // (qw.Ev*: each of that family of globals.)
        const QStringList exprs = expandFamilies(node.text(QStringLiteral("expression")).split(QLatin1Char(';'), Qt::SkipEmptyParts), &solution);
        // The unit: one for each expression ("eV; nm"), or one for each it
        // is a unit of ("qw.Ex; qw.Eb; qw.gX" in eV: the energies in it).
        const QStringList units = unitText.split(QLatin1Char(';'));
        const bool each = exprs.size() > 1 && units.size() == exprs.size();
        int fitted = 0, index = -1;
        QStringList problems;
        for (const QString& raw : exprs) {
            ++index;
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
            // (A global of a unit its dimension cannot say - emw.loss, dB/m -
            // in it, or over another length: dB/cm.)
            QString warning, asked = each ? units[index].trimmed() : unitText.trimmed();
            DisplayUnit du;
            if (exprs.size() > 1 && !each && !asked.isEmpty()) {
                // One unit for several: those it is a unit of in it, the
                // rest in their own.
                QString notOfIt;
                du = displayUnitOf(solution, text, asked, e.dim(), &notOfIt);
                if (notOfIt.isEmpty() && du.name == asked) ++fitted;
                else du = displayUnitOf(solution, text, QString(), e.dim());
            } else {
                du = displayUnitOf(solution, text, asked, e.dim(), &warning);
            }
            if (du.name.isEmpty()) du = quotientUnit(text, e.dim(), solution.scope());
            out.values << DerivedValue{item.name, e.constant(), e.dim(), shown(e.constant(), e.dim(), du)};
            if (!warning.isEmpty() && out.error.isEmpty()) out.error = warning;
        }
        if (exprs.size() > 1 && !each && !unitText.trimmed().isEmpty() && fitted == 0 && problems.isEmpty())
            out.error = tr("%1 is a unit of none of them").arg(unitText.trimmed());
        if (!problems.isEmpty()) out.error = problems.join(QLatin1String("; "));
        return out;
    }
    if (type == QLatin1String("harmonics")) {
        // A global's harmonics in time: over the last whole periods of the
        // fundamental f0 its Fourier coefficients, (2/T) ∫ y e^(-j n 2π f0 t)
        // dt by the trapezoids of its snapshots (its first at the window's
        // start, interpolated) - spectrally exact for evenly spaced times
        // over whole periods -: its mean, each harmonic's amplitude, HDn =
        // 20 log10(|Hn|/|H1|) in dBc, THD = √(Σ |Hn|²)/|H1|.
        const QString text = node.text(QStringLiteral("expression")).trimmed();
        if (solution.frequencyDomain || solution.snapshotCount() < 3) {
            out.error = tr("Harmonics: a step in time's solution, three output times at least");
            return out;
        }
        Dim dim;
        QString why;
        const std::vector<double> y = solution.globalSeries(text, &dim, &why);
        if (y.empty()) {
            out.error = tr("Harmonics: %1").arg(why);
            return out;
        }
        const std::optional<double> f0 = evaluateConstant(node.text(QStringLiteral("f0")), solution.scope(), Dim::of(0, 0, -1), &why);
        if (!f0 || !(*f0 > 0)) {
            out.error = tr("Harmonics: its fundamental %1").arg(f0 ? tr("must be more than 0") : why);
            return out;
        }
        const int count = std::clamp(node.integer(QStringLiteral("count")), 1, 50), periods = std::max(node.integer(QStringLiteral("periods")), 1);
        const int last = solution.snapshotCount() - 1;
        const double tb = solution.snapshotTime(last), ta = tb - periods / *f0;
        if (ta < solution.snapshotTime(0) - 1e-9 * (tb - solution.snapshotTime(0))) {
            out.error = tr("Harmonics: its %1 periods of %2 Hz are longer than the solution's times").arg(periods).arg(*f0);
            return out;
        }
        std::vector<std::pair<double, double>> pts;
        for (int k = 0; k <= last; ++k) {
            const double t = solution.snapshotTime(k);
            if (t < ta) continue;
            if (pts.empty() && t > ta && k > 0) {
                const double t0 = solution.snapshotTime(k - 1), r = (ta - t0) / (t - t0);
                pts.emplace_back(ta, y[std::size_t(k - 1)] + r * (y[std::size_t(k)] - y[std::size_t(k - 1)]));
            }
            pts.emplace_back(t, y[std::size_t(k)]);
        }
        if (pts.size() < 3) {
            out.error = tr("Harmonics: fewer than three output times in its last %1 periods").arg(periods);
            return out;
        }
        const double T = tb - ta, w = 2 * M_PI * *f0;
        double mean = 0;
        std::vector<std::complex<double>> c(std::size_t(count) + 1, 0.0);
        for (std::size_t k = 1; k < pts.size(); ++k) {
            const auto [t0, y0] = pts[k - 1];
            const auto [t1, y1] = pts[k];
            const double h = t1 - t0;
            mean += 0.5 * (y0 + y1) * h / T;
            for (int n = 1; n <= count; ++n)
                c[std::size_t(n)] += (2 / T) * 0.5 * h * (y0 * std::exp(std::complex<double>(0, -n * w * t0)) + y1 * std::exp(std::complex<double>(0, -n * w * t1)));
        }
        add(QStringLiteral("mean"), mean, dim);
        double rest = 0;
        for (int n = 1; n <= count; ++n) {
            add(QStringLiteral("H%1").arg(n), std::abs(c[std::size_t(n)]), dim);
            if (n > 1) rest += std::norm(c[std::size_t(n)]);
        }
        const double h1 = std::abs(c[1]);
        for (int n = 2; n <= count; ++n)
            out.values << DerivedValue{QStringLiteral("HD%1").arg(n), h1 > 0 ? 20 * std::log10(std::abs(c[std::size_t(n)]) / h1) : NAN, Dim(),
                                       h1 > 0 ? QStringLiteral("%1 dBc").arg(20 * std::log10(std::abs(c[std::size_t(n)]) / h1), 0, 'f', 2) : QStringLiteral("-")};
        if (count > 1) out.values << DerivedValue{QStringLiteral("THD"), h1 > 0 ? std::sqrt(rest) / h1 : NAN, Dim(),
                                                  h1 > 0 ? QStringLiteral("%1 %").arg(100 * std::sqrt(rest) / h1, 0, 'g', 4) : QStringLiteral("-")};
        out.note = tr("over %1 periods of %2 Hz, %3 output times").arg(periods).arg(*f0).arg(pts.size());
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
        const QString tag = modeAnalysisOf(solution, node);
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
    if (type == QLatin1String("perturbation")) {
        // To the first order, each mode at 1 W (∫ Sz over the physics' all):
        // Δneff = (c ε0/2) ∫ n Δn |E|², the power's Δα = (c ε0/2) ∫ n Δα |E|²
        // over the domains chosen.
        const QString tag = modeAnalysisOf(solution, node);
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
        const Expression sz = Expression::compile(tag + QStringLiteral(".Sz"), solution.scope());
        const Over power = overDomains(solution, sz, every, false);
        if (!sz.isValid() || !power.any || !(std::abs(power.integral) > 0)) {
            out.error = tr("%1's mode is not defined in them").arg(tag);
            return out;
        }
        constexpr double c0eps0 = 299792458.0 * 8.8541878188e-12;
        const QString weight = tag + QStringLiteral(".n*") + tag + QStringLiteral(".normE^2");
        // (c ε0/2) ∫ n x |E|² over ∫ Sz of \a text, which must be of \a dim.
        auto overlap = [&node, &solution, &out, &weight, &picked, &power](const QString& key, const Dim& dim) -> std::optional<double> {
            const QString text = node.text(key).trimmed();
            if (text.isEmpty()) return std::nullopt;
            const Expression x = Expression::compile(text, solution.scope());
            if (!x.isValid()) {
                if (out.error.isEmpty()) out.error = tr("%1: %2").arg(text, x.error());
                return std::nullopt;
            }
            if (x.dim() != dim) {
                if (out.error.isEmpty()) out.error = tr("%1 is in %2, not %3").arg(text, dimName(x.dim()), dimName(dim));
                return std::nullopt;
            }
            const Expression e = Expression::compile(QStringLiteral("(%1)*%2").arg(text, weight), solution.scope());
            const Over o = overDomains(solution, e, picked, false);
            return o.any ? 0.5 * c0eps0 * o.integral / power.integral : 0.0;
        };
        const std::optional<double> dneff = overlap(QStringLiteral("dn"), Dim()), dalpha = overlap(QStringLiteral("dalpha"), Dim::of(-1));
        if (!out.error.isEmpty()) return out;
        if (dneff) add(QStringLiteral("dneff"), *dneff, Dim(), false);
        if (dalpha) {
            add(QStringLiteral("dalpha"), *dalpha, Dim::of(-1), false);
            const double db = 10 / std::log(10.0) * *dalpha;
            add(QStringLiteral("dloss"), db, Dim::of(-1), false);
            out.values.back().text = formatNumber(db / 100, 4) + QStringLiteral(" dB/cm");
        }
        const QString lengthText = node.text(QStringLiteral("length")).trimmed();
        if (dneff && !lengthText.isEmpty()) {
            QString err;
            const std::optional<double> L = evaluateConstant(lengthText, solution.parameters().scope, Dim::length(), &err);
            const std::optional<double> lambda = solution.global(tag + QStringLiteral(".lambda0"));
            if (!L || !lambda) {
                out.error = L ? tr("%1 has no wavelength").arg(tag) : tr("the length %1: %2").arg(lengthText, err);
                return out;
            }
            add(QStringLiteral("dphi"), 2 * M_PI / *lambda * *dneff * *L, Dim(), false);
            out.values.back().text = formatNumber(out.values.back().value, 4) + QStringLiteral(" rad");
        }
        const QString bias = node.text(QStringLiteral("bias")).trimmed();
        if (!bias.isEmpty()) {
            const Expression v = Expression::compile(bias, solution.parameters().scope);
            if (!v.isValid() || !v.isConstant()) {
                out.error = tr("the bias %1: %2").arg(bias, v.isValid() ? tr("not a parameter") : v.error());
                return out;
            }
            add(bias, v.constant(), v.dim(), false);
        }
        return out;
    }
    if (type == QLatin1String("overlap")) {
        // The mode overlapped with another field (Snyder and Love's, the
        // overlap of Lumerical MODE): Re[(∫E1×H2*)(∫E2×H1*)/∫E1×H1*]/Re
        // ∫E2×H2* - the power coupled from the other into the mode at a butt
        // joint, reflections aside -, over the mode's domains (its perfectly
        // matched layers left out), the other's own flux its own (what falls
        // outside the cross-section lost).
        using cd = std::complex<double>;
        const QString tag = modeAnalysisOf(solution, node);
        const int fi = solution.fieldIndex(tag);
        if (fi < 0 || !solution.modeAnalysis || solution.fields()[std::size_t(fi)].type != QLatin1String("modes")) {
            out.error = tr("there is no mode analysis %1 solved").arg(tag);
            return out;
        }
        // (Another mode is another snapshot: selected for it, then again the
        // one it was - as the solutions are each selected to be evaluated.)
        Solution& s = const_cast<Solution&>(solution);
        const double scale = s.topology().unitScale;
        auto lengths = [&node, &s, scale, &out](const QString& key, double* xy) {
            const QStringList two = node.pair(key);
            Expression::Options options;
            options.lengthUnit = scale;
            options.numbersAreLengths = true;
            for (int k = 0; k < 2; ++k) {
                const QString text = two.value(k).trimmed().isEmpty() ? QStringLiteral("0") : two.value(k);
                const Expression e = Expression::compile(text, s.parameters().scope, options);
                if (!e.isValid() || !e.isConstant()) {
                    out.error = tr("%1: %2").arg(text, e.isValid() ? tr("not a constant") : e.error());
                    return false;
                }
                xy[k] = scale * e.constant();
            }
            return true;
        };
        double offset[2] = {0, 0};
        if (!lengths(QStringLiteral("offset"), offset)) return out;
        const bool moved = offset[0] != 0 || offset[1] != 0;
        // The mode at its quadrature points (m), each's weight (m²).
        struct At {
            int t;
            double xi, eta, w;
            QPointF x;
            cd E[3], H[3];
        };
        std::vector<At> pts;
        const std::vector<int> pml = pmlDomainsOf(s, tag);
        const Field& fld = s.fields()[std::size_t(fi)];
        const Space& space = *fld.space;
        const Mesh& mesh = s.mesh();
        auto inLayer = [&pml](int d) { return std::find(pml.begin(), pml.end(), d) != pml.end(); };
        PointEvaluator ev(s);
        for (int t = 0; t < int(mesh.triangles.size()); ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (d < 0 || std::size_t(d) >= fld.domains.size() || !fld.domains[std::size_t(d)] || inLayer(d)) continue;
            for (const auto& q : triangleRule(space.curved(t) ? 5 : 4).points) {
                ev.moveTo(t, q[0], q[1]);
                const detail::ModeFields m = detail::modeFieldsAt(ev, fi);
                if (!m.defined) continue;
                const Space::Map map = space.map(t, q[0], q[1]);
                At a{t, q[0], q[1], q[2] * std::abs(map.det), map.x, {}, {}};
                for (int k = 0; k < 3; ++k) {
                    a.E[k] = m.E[k];
                    a.H[k] = m.H[k];
                }
                pts.push_back(a);
            }
        }
        if (pts.empty()) {
            out.error = tr("%1's mode is not defined anywhere").arg(tag);
            return out;
        }
        // The other field at each point, moved by the offset (its value at x -
        // offset), its own ∫E2×H2*·z and ∫|E2t|².
        std::vector<std::array<cd, 6>> other(pts.size());
        cd n2 = 0;
        double e2 = 0;
        QString what;
        const QString with = node.text(QStringLiteral("with"));
        if (with == QLatin1String("mode")) {
            const int wanted = node.integer(QStringLiteral("other"));
            int was = s.selected(), k = -1;
            for (int i = 0; i < s.snapshotCount(); ++i) {
                const bool here = std::abs(s.snapshotFrequency(i) - s.frequency()) <= 1e-9 * std::max(1.0, std::abs(s.frequency()));
                if (here && was < 0 && s.snapshotMode(i) == s.mode()) was = i;   // (the fields as they are: their own snapshot)
                if (here && s.snapshotMode(i) == wanted) k = i;
            }
            if (k < 0 || was < 0) {
                out.error = k < 0 ? tr("there is no mode %1 at its frequency (it found %2)").arg(wanted).arg(s.snapshotCount())
                                  : tr("its mode's own snapshot is not found");
                return out;
            }
            s.select(was);   // (the first, as it is)
            s.select(k);
            const Locator locator(mesh);
            for (std::size_t i = 0; i < pts.size(); ++i) {
                // (Its own flux at its points unmoved; the overlap's at x - offset.)
                ev.moveTo(pts[i].t, pts[i].xi, pts[i].eta);
                const detail::ModeFields own = detail::modeFieldsAt(ev, fi);
                if (own.defined) {
                    n2 += pts[i].w * (own.E[0] * std::conj(own.H[1]) - own.E[1] * std::conj(own.H[0]));
                    e2 += pts[i].w * (std::norm(own.E[0]) + std::norm(own.E[1]));
                }
                if (moved) {
                    int t = -1;
                    double xi = 0, eta = 0;
                    const QPointF p((pts[i].x.x() - offset[0]) / scale, (pts[i].x.y() - offset[1]) / scale);
                    if (!locator.locate(p, &t, &xi, &eta)) continue;
                    ev.moveTo(t, xi, eta);
                    if (inLayer(ev.domain())) continue;
                }
                const detail::ModeFields m = moved ? detail::modeFieldsAt(ev, fi) : own;
                if (!m.defined) continue;
                for (int c = 0; c < 3; ++c) {
                    other[i][std::size_t(c)] = m.E[c];
                    other[i][std::size_t(c + 3)] = m.H[c];
                }
            }
            s.select(was);
            what = tr("mode %1").arg(wanted);
        } else if (with == QLatin1String("file")) {
            QString path = node.text(QStringLiteral("file")).trimmed();
            if (path.isEmpty()) {
                out.error = tr("choose its mode profile file (.mat)");
                return out;
            }
            if (QFileInfo(path).isRelative()) {
                if (s.model().fileName.isEmpty()) {
                    out.error = tr("%1 is relative to the model's folder: save the model first").arg(path);
                    return out;
                }
                path = QFileInfo(s.model().fileName).absoluteDir().absoluteFilePath(path);
            }
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                out.error = tr("%1 cannot be read: %2").arg(QDir::toNativeSeparators(path), file.errorString());
                return out;
            }
            QString why;
            const std::optional<ModeProfile> prof = readProfileMat(file.readAll(), std::max(1, node.integer(QStringLiteral("filemode"))), &why);
            if (!prof) {
                out.error = tr("%1: %2").arg(QFileInfo(path).fileName(), why);
                return out;
            }
            for (std::size_t i = 0; i < pts.size(); ++i) {
                cd E[3], H[3];
                if (!prof->at(QPointF(pts[i].x.x() - offset[0], pts[i].x.y() - offset[1]), E, H)) continue;
                for (int c = 0; c < 3; ++c) {
                    other[i][std::size_t(c)] = E[c];
                    other[i][std::size_t(c + 3)] = H[c];
                }
            }
            n2 = prof->flux();
            // (Its |E| alone's ∫ over its grid, by the trapezoid rule.)
            const int nx = prof->nx(), ny = prof->ny();
            for (int j = 0; j < ny; ++j) {
                const double wy = 0.5 * ((j > 0 ? prof->y[std::size_t(j)] - prof->y[std::size_t(j - 1)] : 0) +
                                         (j + 1 < ny ? prof->y[std::size_t(j + 1)] - prof->y[std::size_t(j)] : 0));
                for (int i = 0; i < nx; ++i) {
                    const double wx = 0.5 * ((i > 0 ? prof->x[std::size_t(i)] - prof->x[std::size_t(i - 1)] : 0) +
                                             (i + 1 < nx ? prof->x[std::size_t(i + 1)] - prof->x[std::size_t(i)] : 0));
                    const std::size_t at = std::size_t(j) * std::size_t(nx) + std::size_t(i);
                    e2 += wx * wy * (std::norm(prof->F[0][at]) + std::norm(prof->F[1][at]));
                }
            }
            what = tr("%1's mode %2").arg(QFileInfo(path).fileName()).arg(prof->mode);
        } else {
            // A Gaussian beam at its waist: E = exp(-r²/w0²) along x or y, H a
            // plane wave's in its medium, (n/η0) z × E.
            QString why;
            const std::optional<double> w0 = evaluateConstant(node.text(QStringLiteral("waist")), s.parameters().scope, Dim::length(), &why);
            if (!w0 || !(*w0 > 0)) {
                out.error = tr("its waist: %1").arg(w0 ? tr("it must be more than 0") : why);
                return out;
            }
            double centre[2] = {0, 0};
            if (!lengths(QStringLiteral("centre"), centre)) return out;
            double n = s.global(tag + QStringLiteral(".neff")).value_or(1.0);
            if (!node.text(QStringLiteral("index")).trimmed().isEmpty()) {
                const std::optional<double> given = evaluateConstant(node.text(QStringLiteral("index")), s.parameters().scope, Dim(), &why);
                if (!given || !(*given > 0)) {
                    out.error = tr("its medium's index: %1").arg(given ? tr("it must be more than 0") : why);
                    return out;
                }
                n = *given;
            }
            const bool alongY = node.text(QStringLiteral("polarization")) == QLatin1String("y");
            constexpr double Eta0 = 376.730313412;
            for (std::size_t i = 0; i < pts.size(); ++i) {
                const double dx = pts[i].x.x() - offset[0] - centre[0], dy = pts[i].x.y() - offset[1] - centre[1];
                const double e = std::exp(-(dx * dx + dy * dy) / (*w0 * *w0));
                other[i][alongY ? 1 : 0] = e;
                other[i][alongY ? 3 : 4] = (alongY ? -1.0 : 1.0) * n / Eta0 * e;
            }
            e2 = M_PI * *w0 * *w0 / 2;
            n2 = n / Eta0 * e2;
            what = tr("a Gaussian of waist %1").arg(formatQuantity(*w0, Dim::length()));
        }
        cd c12 = 0, c21 = 0, n1 = 0, e12 = 0;
        double e1 = 0;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const cd* E1 = pts[i].E;
            const cd* H1 = pts[i].H;
            const auto& o = other[i];
            const double w = pts[i].w;
            c12 += w * (E1[0] * std::conj(o[4]) - E1[1] * std::conj(o[3]));
            c21 += w * (o[0] * std::conj(H1[1]) - o[1] * std::conj(H1[0]));
            n1 += w * (E1[0] * std::conj(H1[1]) - E1[1] * std::conj(H1[0]));
            e12 += w * (E1[0] * std::conj(o[0]) + E1[1] * std::conj(o[1]));
            e1 += w * (std::norm(E1[0]) + std::norm(E1[1]));
        }
        if (!(std::abs(n1) > 0) || !(std::abs(n2.real()) > 0) || !(e1 > 0) || !(e2 > 0)) {
            out.error = tr("the mode or %1 carries no power").arg(what);
            return out;
        }
        const double eta = (c12 * c21 / n1).real() / n2.real();
        add(QStringLiteral("overlap"), eta, Dim(), false);
        add(QStringLiteral("loss"), eta > 0 ? -10 * std::log10(eta) : INFINITY, Dim(), false);
        out.values.back().text = eta > 0 ? formatNumber(-10 * std::log10(eta), 4) + QStringLiteral(" dB") : tr("no coupling");
        add(QStringLiteral("overlapE"), std::norm(e12) / (e1 * e2), Dim(), false);
        out.note = moved ? tr("with %1 moved by (%2, %3)").arg(what, formatQuantity(offset[0], Dim::length()), formatQuantity(offset[1], Dim::length()))
                         : tr("with %1").arg(what);
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
        const Dim perMetre = Dim::of(-1);
        const Dim henry = Dim::of(2, 1, -2, -2);
        if (lineTerminals(node).size() > 1) {
            // Coupled lines: their matrices, each mode's Z, γ, n_eff, εeff.
            const CoupledLines cl = coupledLines(solution, node);
            if (!cl.error.isEmpty()) {
                out.error = cl.error;
                return out;
            }
            const int n = int(cl.names.size());
            auto matrix = [&](const QString& letter, const CoupledLines::Matrix& m, const Dim& dim) {
                for (int i = 0; i < n; ++i)
                    for (int j = 0; j < n; ++j)
                        add(letter + QLatin1Char('_') + cl.names[i] + QLatin1Char('_') + cl.names[j], m[std::size_t(i)][std::size_t(j)], dim, false);
            };
            matrix(QStringLiteral("C"), cl.C, Dim::capacitance() * perMetre);
            matrix(QStringLiteral("C0"), cl.C0, Dim::capacitance() * perMetre);
            matrix(QStringLiteral("L"), cl.L, henry * perMetre);
            if (cl.rlgc) {
                matrix(QStringLiteral("R"), cl.R, Dim::resistance() * perMetre);
                matrix(QStringLiteral("G"), cl.G, Dim::conductance() * perMetre);
            }
            auto complexText = [](std::complex<double> z, const Dim& dim) {
                const QString re = formatQuantity(z.real(), dim);
                return std::abs(z.imag()) <= 1e-12 * std::abs(z) ? re
                                                                  : QStringLiteral("%1 %2 j%3").arg(re, z.imag() < 0 ? QStringLiteral("-") : QStringLiteral("+"),
                                                                                                    formatQuantity(std::abs(z.imag()), dim));
            };
            for (const LineMode& m : cl.modes) {
                const QString t = QLatin1Char('_') + m.name;
                add(QStringLiteral("Z0") + t, m.Z.real(), Dim::resistance(), false);
                out.values.back().text = complexText(m.Z, Dim::resistance());
                if (cl.rlgc) add(QStringLiteral("Z0") + t + QStringLiteral("_im"), m.Z.imag(), Dim::resistance(), false);
                add(QStringLiteral("n_eff") + t, m.nEff, Dim(), false);
                if (cl.rlgc) {
                    add(QStringLiteral("alpha") + t, m.alphaDb, perMetre, false);
                    out.values.back().text = formatNumber(m.alphaDb, 4) + QStringLiteral(" dB/m");
                    add(QStringLiteral("beta") + t, m.gamma.imag(), perMetre, false);
                    out.values.back().text = formatNumber(m.gamma.imag(), 4) + QStringLiteral(" rad/m");
                    add(QStringLiteral("epsilon_eff") + t, m.nEff * m.nEff, Dim(), false);
                }
                add(QStringLiteral("epsilon_eff_qs") + t, m.epsEffQs, Dim(), false);
                add(QStringLiteral("v") + t, m.nEff > 0 ? C0 / m.nEff : NAN, Dim::of(1, 0, -1), false);
            }
            if (cl.symmetricPair) {
                // Differential: 2 Z_odd; common: Z_even / 2.
                const std::complex<double> zd = 2.0 * cl.modes[1].Z, zc = 0.5 * cl.modes[0].Z;
                add(QStringLiteral("Zdiff"), zd.real(), Dim::resistance(), false);
                out.values.back().text = complexText(zd, Dim::resistance());
                if (cl.rlgc) add(QStringLiteral("Zdiff_im"), zd.imag(), Dim::resistance(), false);
                add(QStringLiteral("Zcm"), zc.real(), Dim::resistance(), false);
                out.values.back().text = complexText(zc, Dim::resistance());
                if (cl.rlgc) add(QStringLiteral("Zcm_im"), zc.imag(), Dim::resistance(), false);
                out.note = tr("a symmetric pair: its even mode (the lines at one voltage) and odd mode (at opposite ones), each line's Z0");
            } else {
                // Each mode's voltages and currents on the conductors, the
                // characteristic impedance matrix.
                for (const LineMode& m : cl.modes)
                    for (int k = 0; k < n; ++k) {
                        const QString t = QLatin1Char('_') + m.name + QLatin1Char('_') + cl.names[k];
                        add(QStringLiteral("V") + t, m.v[std::size_t(k)].real(), Dim(), false);
                        out.values.back().text = complexText(m.v[std::size_t(k)], Dim());
                        add(QStringLiteral("V") + t + QStringLiteral("_im"), m.v[std::size_t(k)].imag(), Dim(), false);
                        add(QStringLiteral("I") + t, m.i[std::size_t(k)].real(), Dim::conductance(), false);
                        out.values.back().text = complexText(m.i[std::size_t(k)], Dim::conductance());
                        add(QStringLiteral("I") + t + QStringLiteral("_im"), m.i[std::size_t(k)].imag(), Dim::conductance(), false);
                    }
                for (int i = 0; i < n; ++i)
                    for (int j = 0; j < n; ++j) {
                        const QString t = QLatin1Char('_') + cl.names[i] + QLatin1Char('_') + cl.names[j];
                        add(QStringLiteral("Zc") + t, cl.Zc[std::size_t(i)][std::size_t(j)].real(), Dim::resistance(), false);
                        out.values.back().text = complexText(cl.Zc[std::size_t(i)][std::size_t(j)], Dim::resistance());
                        add(QStringLiteral("Zc") + t + QStringLiteral("_im"), cl.Zc[std::size_t(i)][std::size_t(j)].imag(), Dim::resistance(), false);
                    }
                out.note = tr("modes 1 to %1 by n_eff, the highest first: V_<mode>_<line> their voltages (the largest 1), I_ their "
                              "currents (A per volt), Zc the characteristic impedance matrix")
                               .arg(n);
            }
            return out;
        }
        if (node.text(QStringLiteral("method")) == QLatin1String("rlgc")) {
            const LineRLGC l = lineRLGC(solution, node);
            if (!l.error.isEmpty()) {
                out.error = l.error;
                return out;
            }
            add(QStringLiteral("R"), l.R, Dim::resistance() * perMetre, false);
            add(QStringLiteral("L"), l.L, henry * perMetre, false);
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
            // n_eff = β/k0 (εeff its square, the conductors' slow wave in
            // it); the quasi-static εeff = C/C0, of the dielectric alone.
            add(QStringLiteral("n_eff"), l.frequency > 0 ? l.gamma.imag() * C0 / (2 * M_PI * l.frequency) : std::sqrt(l.epsEff), Dim(), false);
            TerminalMatrices t0;
            QString why;
            if (terminalMatrices(solution, tag, lineTerminals(node), true, &t0, &why) && !t0.C.empty() && t0.C[0][0] > 0)
                add(QStringLiteral("epsilon_eff_qs"), l.C / (t0.C[0][0] / solution.thickness()), Dim(), false);
            else
                out.error = tr("its quasi-static εeff: %1").arg(why);
            out.note = tr("epsilon_eff is (β/k0)², n_eff squared - with the conductors' loss slowing the wave; epsilon_eff_qs is C/C0, "
                          "the dielectric's alone");
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
        TerminalMatrices t0;
        const double c0 = terminalMatrices(solution, tag, {terminal}, true, &t0, &why) && !t0.C.empty() ? t0.C[0][0] / d : NAN;
        if (!std::isfinite(c0) || c0 <= 0) {
            out.error = why.isEmpty() ? tr("the line's capacitance in vacuum could not be found") : why;
            return out;
        }
        add(QStringLiteral("C"), c, Dim::capacitance() * perMetre, false);
        add(QStringLiteral("C0"), c0, Dim::capacitance() * perMetre, false);
        add(QStringLiteral("epsilon_eff"), c / c0, Dim(), false);
        add(QStringLiteral("Z0"), 1.0 / (C0 * std::sqrt(c * c0)), Dim::resistance(), false);
        add(QStringLiteral("L"), 1.0 / (C0 * C0 * c0), Dim::of(2, 1, -2, -2) * perMetre, false);
        add(QStringLiteral("v"), C0 / std::sqrt(c / c0), Dim::of(1, 0, -1), false);
        add(QStringLiteral("n_eff"), std::sqrt(c / c0), Dim(), false);
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
        // A Laser L-I step's points, by number when no sweep has them (the
        // last, whatever was asked: the update check of 110e8b9); a Valence
        // Bands step's k points too (the check of a4ff84a, 6.1).
        const bool numbered = s.biasAxis || (s.frequencyDomain && s.photonAxis == 3);
        if (!all && numbered && n == 1 && point > 0 && at.trimmed().isEmpty()) pick = std::min(point, snaps) - 1;
        // Modes: at that frequency (the last when none is asked), the mode asked.
        bool several = false;
        if (s.modeAnalysis && s.eigenfrequencies) {
            // A resonator's, each at its own frequency: the mode asked, the
            // first when none is (a frequency asked: the nearest's).
            several = true;
            if (mode > 0) {
                for (int k = 0; k < snaps; ++k)
                    if (s.snapshotMode(k) == mode) pick = k;
            } else if (at.trimmed().isEmpty()) {
                pick = 0;
            }
        } else if (s.modeAnalysis) {
            for (int k = 1; k < snaps; ++k) several = several || s.snapshotFrequency(k) != s.snapshotFrequency(0);
            // (None asked: the one its step chose, the first unless it said.)
            const double f = s.snapshotFrequency(pick);
            int first = -1, chosen = -1;
            for (int k = 0; k < snaps; ++k)
                if (s.snapshotFrequency(k) == f) {
                    if (first < 0) first = k;
                    if (chosen < 0 && s.snapshotChosen(k)) chosen = k;
                    if (mode > 0 && s.snapshotMode(k) == mode) pick = k;
                }
            if (mode <= 0) pick = chosen >= 0 ? chosen : first >= 0 ? first : pick;
            else if (s.snapshotMode(pick) != mode && first >= 0) pick = first;
        }
        for (int k = 0; k < snaps; ++k) {
            if (!all && k != pick) continue;
            // (All, a mode asked: that mode alone at each - or the one chosen at each.)
            if (all && mode > 0 && s.modeAnalysis && s.snapshotMode(k) != mode) continue;
            if (all && mode < 0 && s.modeAnalysis && !s.snapshotChosen(k)) continue;
            QString when = s.snapshotText(k);
            const QString mode = (s.quantumStates ? QStringLiteral("state %1") : QStringLiteral("mode %1")).arg(s.snapshotMode(k));
            if (s.modeAnalysis) when = several ? when + QStringLiteral(", ") + mode : mode;
            out.push_back({p, k, s.snapshotValue(k), s.snapshotMode(k), s.label.isEmpty() ? when : s.label + QStringLiteral(", ") + when});
        }
    }
    return out;
}

void perturbationAlongSweep(const SolutionSet& set, const std::vector<Instance>& at, const Node& node, std::vector<DerivedResult>& values)
{
    const QString bias = node.text(QStringLiteral("bias")).trimmed(), lengthText = node.text(QStringLiteral("length")).trimmed();
    auto find = [](const DerivedResult& r, const QString& name) -> const DerivedValue* {
        for (const DerivedValue& v : r.values)
            if (v.name == name) return &v;
        return nullptr;
    };
    // Each mode's first point: its reference.
    std::map<int, std::size_t> first;
    for (std::size_t i = 0; i < at.size() && i < values.size(); ++i) {
        const DerivedValue* dn = find(values[i], QStringLiteral("dneff"));
        if (!dn) continue;
        const auto ref = first.find(at[i].mode);
        if (ref == first.end()) {
            first[at[i].mode] = i;
            continue;
        }
        const DerivedValue* dn0 = find(values[ref->second], QStringLiteral("dneff"));
        const Solution& s = *set.solutions[std::size_t(at[i].point)];
        const double frequency = at[i].snapshot >= 0 ? s.snapshotFrequency(at[i].snapshot) : s.frequency();
        if (!(frequency > 0)) continue;
        const double lambda = 299792458.0 / frequency, change = dn->value - dn0->value;
        if (!lengthText.isEmpty()) {
            const std::optional<double> L = evaluateConstant(lengthText, s.parameters().scope, Dim::length());
            if (L) {
                const double phi = 2 * M_PI / lambda * change * *L;
                values[i].values << DerivedValue{QStringLiteral("dphi_sweep"), phi, Dim(), formatNumber(phi, 4) + QStringLiteral(" rad")};
            }
        }
        const DerivedValue* v = bias.isEmpty() ? nullptr : find(values[i], bias);
        const DerivedValue* v0 = bias.isEmpty() ? nullptr : find(values[ref->second], bias);
        if (v && v0 && v->value != v0->value && change != 0) {
            // Vπ L: the bias moving the phase by π over a length L, times L.
            const double vpil = lambda * std::abs(v->value - v0->value) / (2 * std::abs(change));
            const Dim dim = v->dim * Dim::length();
            // (In cm as a voltage's is: a bias without a unit was said in m -
            // "0.003091 (bias)·m" for 0.3091 V·cm.)
            values[i].values << DerivedValue{QStringLiteral("VpiL"), vpil, dim,
                                             v->dim == Dim::voltage() ? formatNumber(vpil * 100, 4) + QStringLiteral(" V·cm")
                                             : v->dim.isNone()        ? formatNumber(vpil * 100, 4) + QStringLiteral(" cm (times the bias's unit)")
                                                                      : formatNumber(vpil * 100, 4) + QStringLiteral(" (%1)·cm").arg(dimName(v->dim))};
        }
    }
}

int modesAsked(const SolutionSet& set, const QString& which)
{
    if (which == QLatin1String("chosen")) return -1;
    if (which != QLatin1String("auto") && !which.isEmpty()) return 0;
    for (const auto& s : set.solutions)
        if (s->modeAnalysis && s->modeChosen) return -1;
    return 0;
}

QString modeAnalysisOf(const Solution& solution, const Node& node)
{
    const QStringList tags = node.list(QStringLiteral("physics"));
    const QString tag = tags.isEmpty() ? QString() : tags.first();
    const int fi = solution.fieldIndex(tag);
    if ((fi >= 0 && solution.fields()[std::size_t(fi)].type == QLatin1String("modes")) || (node.props.contains(QStringLiteral("physics")) && !tag.isEmpty()))
        return tag;
    for (const Field& f : solution.fields())
        if (f.type == QLatin1String("modes")) return f.tag;
    return tag;
}

std::vector<int> pmlDomainsOf(const Solution& solution, const QString& physics)
{
    std::vector<int> out;
    const Node* phys = solution.model().find(physics);
    if (!phys) return out;
    for (const Node& f : phys->children) {
        if (!f.enabled || !f.type.endsWith(QLatin1String("/pml"))) continue;
        QString why;
        for (int d : resolveSelection(solution.topology(), solution.model(), Level::Domain, f.selection(), &why)) out.push_back(d);
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
