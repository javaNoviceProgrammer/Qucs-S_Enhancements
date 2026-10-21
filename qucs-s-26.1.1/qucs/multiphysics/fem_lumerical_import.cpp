/*
 * fem_lumerical_import.cpp - Lumerical's projects and scripts made
 *                            multiphysics models: MODE's FDE (.lms, .lsf),
 *                            CHARGE's (.lsf; .ldev in fem_lumerical.cpp),
 *                            an MQW stack's (.lsf's mqwindex)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * A scene's structures are cut by the solver's plane (MODE's FDE region's,
 * CHARGE's simulation region's) and clipped to its region; where they
 * overlap the one of the lower mesh order wins (Lumerical's rule; of the
 * same, the later): their materials are given in that order, each on its
 * object's domains, the last given winning.
 */
#include "fem_lumerical_scene.h"

#include "fem_geometry.h"
#include "fem_model.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <functional>
#include <optional>

namespace qucs_s::fem::lumerical {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("LumericalImport", text);
}

constexpr double C0 = 299792458.0, Q = 1.602176634e-19, H = 6.62607015e-34;

QString num(double v)
{
    if (!std::isfinite(v)) return QStringLiteral("0");
    const QString s = QString::number(v, 'g', 10);
    return s == QLatin1String("-0") ? QStringLiteral("0") : s;
}

/// "v[unit]", times (T/300 K)^η where η is not 0 (a parameter of the
/// temperature, as the library's).
QString ofT(double v, const QString& unit, double eta)
{
    QString t = unit.isEmpty() ? num(v) : QStringLiteral("%1[%2]").arg(num(v), unit);
    if (eta != 0 && std::isfinite(eta)) t += QStringLiteral("*(T/300[K])^(%1)").arg(num(eta));
    return t;
}

// --- The cross-section --------------------------------------------------

/// The solver's plane: its normal (0 x, 1 y, 2 z) and where it is, its
/// axes (the model's x and y), its region in them.
struct Plane {
    int normal = 2, u = 0, v = 1;
    double at = 0;
    double u0 = -INFINITY, v0 = -INFINITY, u1 = INFINITY, v1 = INFINITY;
    bool contains(double a, double b) const { return a >= u0 && a <= u1 && b >= v0 && b <= v1; }
};

/// A box along an axis as Lumerical's objects have it: its middle and span
/// or its ends, as they were set in turn.
std::pair<double, double> span(const Object& o, const QString& a, double otherwise)
{
    double lo = -otherwise / 2, hi = otherwise / 2;
    for (const auto& [k, v] : o.history) {
        if (v.kind != Value::Matrix || v.m.empty()) continue;
        const double x = v.m[0];
        if (k == a) {
            const double h = (hi - lo) / 2;
            lo = x - h;
            hi = x + h;
        } else if (k == a + QStringLiteral(" span")) {
            const double c = (lo + hi) / 2;
            lo = c - x / 2;
            hi = c + x / 2;
        } else if (k == a + QStringLiteral(" min"))
            lo = x;
        else if (k == a + QStringLiteral(" max"))
            hi = x;
    }
    if (hi < lo) std::swap(lo, hi);
    return {lo, hi};
}

/// A structure cut by the plane: a rectangle, a polygon or a circle (the
/// plane's axes, metres); its name, material, mesh order, place in turn.
struct Shape {
    QString name, label, material;
    enum Kind { Rect, Poly, Circle } kind = Rect;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // a rectangle's; a polygon's, a circle's box
    std::vector<QPointF> points;
    double cx = 0, cy = 0, r = 0;
    int order = 2, turn = 0;
    QString index;                           // an object defined dielectric's
};

/// A rotation's matrix: about the axes in turn (0 none, 1 x, 2 y, 3 z; or
/// their names), by the angles (degrees).
std::array<double, 9> rotation(const Object& o)
{
    std::array<double, 9> R = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const char* axes[3] = {"first axis", "second axis", "third axis"};
    for (int k = 0; k < 3; ++k) {
        const Value* a = o.prop(QString::fromLatin1(axes[k]));
        if (!a) continue;
        int axis = 0;
        if (a->kind == Value::Text) {
            const QString t = a->s.trimmed().toLower();
            axis = t == QLatin1String("x") ? 1 : t == QLatin1String("y") ? 2 : t == QLatin1String("z") ? 3 : 0;
        } else
            axis = whole(a->toNumber(), 0, 3, 0);
        if (axis < 1 || axis > 3) continue;
        const double th = o.number(QStringLiteral("rotation %1").arg(k + 1), 0) * M_PI / 180;
        if (th == 0) continue;
        const double c = std::cos(th), s = std::sin(th);
        std::array<double, 9> r = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const int i = axis == 1 ? 1 : 0, j = axis == 3 ? 1 : 2;   // the plane turned: (y, z), (x, z), (x, y)
        r[std::size_t(i * 3 + i)] = c;
        r[std::size_t(j * 3 + j)] = c;
        r[std::size_t(i * 3 + j)] = axis == 2 ? s : -s;
        r[std::size_t(j * 3 + i)] = axis == 2 ? -s : s;
        std::array<double, 9> out{};
        for (int a1 = 0; a1 < 3; ++a1)
            for (int b1 = 0; b1 < 3; ++b1)
                for (int k1 = 0; k1 < 3; ++k1) out[std::size_t(a1 * 3 + b1)] += r[std::size_t(a1 * 3 + k1)] * R[std::size_t(k1 * 3 + b1)];
        R = out;
    }
    return R;
}

std::optional<Shape> sectionOf(const Object& o, const Plane& p, QStringList& notes)
{
    Shape s;
    s.name = o.name();
    s.material = o.text(QStringLiteral("material"));
    s.index = o.text(QStringLiteral("index"));
    const QLatin1String axes[3] = {QLatin1String("x"), QLatin1String("y"), QLatin1String("z")};
    const double tol = 1e-12;
    if (o.kind == QLatin1String("rect")) {
        std::pair<double, double> b[3];
        for (int a = 0; a < 3; ++a) b[a] = span(o, axes[a], 1e-6);
        if (p.at < b[p.normal].first - tol || p.at > b[p.normal].second + tol) return std::nullopt;
        s.kind = Shape::Rect;
        s.x0 = b[p.u].first;
        s.x1 = b[p.u].second;
        s.y0 = b[p.v].first;
        s.y1 = b[p.v].second;
        return s;
    }
    if (o.kind == QLatin1String("poly")) {
        const Value* V = o.prop(QStringLiteral("vertices"));
        if (!V || V->kind != Value::Matrix || V->cols != 2 || V->rows < 3) {
            notes << tr("%1: a polygon without its vertices (an N x 2 matrix): left out").arg(s.name);
            return std::nullopt;
        }
        const double c[3] = {o.number(QStringLiteral("x"), 0), o.number(QStringLiteral("y"), 0), o.number(QStringLiteral("z"), 0)};
        // (Its extrusion along its own z, about its middle.)
        const std::pair<double, double> w = span(o, QStringLiteral("z"), 1e-6);
        const std::array<double, 9> R = rotation(o);
        const double e[3] = {R[2], R[5], R[8]};
        if (std::abs(e[p.normal]) < 0.999) {
            notes << tr("%1: a polygon extruded along the plane, not across it: left out").arg(s.name);
            return std::nullopt;
        }
        const double n0 = c[p.normal] + e[p.normal] * (w.first - c[2]), n1 = c[p.normal] + e[p.normal] * (w.second - c[2]);
        if (p.at < std::min(n0, n1) - tol || p.at > std::max(n0, n1) + tol) return std::nullopt;
        s.kind = Shape::Poly;
        for (int i = 0; i < V->rows; ++i) {
            const double lx = V->m[std::size_t(i)], ly = V->m[std::size_t(V->rows + i)];
            double g[3];
            for (int a = 0; a < 3; ++a) g[a] = c[a] + R[std::size_t(a * 3)] * lx + R[std::size_t(a * 3 + 1)] * ly;
            s.points.emplace_back(g[p.u], g[p.v]);
        }
        return s;
    }
    if (o.kind == QLatin1String("pyramid")) {
        // A frustum along z: its spans in x and y at z min (bottom) and z max
        // (top), linear between. Cut across z: a rectangle; along it, the
        // trapezoid where the other axis' span reaches the plane.
        const std::array<double, 9> R = rotation(o);
        if (std::abs(R[0] - 1) + std::abs(R[4] - 1) + std::abs(R[8] - 1) > 1e-9) {
            notes << tr("%1: a pyramid turned: left out").arg(s.name);
            return std::nullopt;
        }
        const double c[2] = {o.number(QStringLiteral("x"), 0), o.number(QStringLiteral("y"), 0)};
        const std::pair<double, double> w = span(o, QStringLiteral("z"), 1e-6);
        const double bottom[2] = {o.number(QStringLiteral("x span bottom"), 1e-6), o.number(QStringLiteral("y span bottom"), 1e-6)};
        const double top[2] = {o.number(QStringLiteral("x span top"), bottom[0]), o.number(QStringLiteral("y span top"), bottom[1])};
        auto half = [&](int a, double t) { return (bottom[a] + (top[a] - bottom[a]) * t) / 2; };
        if (p.normal == 2) {
            if (p.at < w.first - tol || p.at > w.second + tol) return std::nullopt;
            const double t = w.second > w.first ? (p.at - w.first) / (w.second - w.first) : 0;
            s.kind = Shape::Rect;
            s.x0 = c[0] - half(0, t);
            s.x1 = c[0] + half(0, t);
            s.y0 = c[1] - half(1, t);
            s.y1 = c[1] + half(1, t);
            return s;
        }
        if (p.v != 2) return std::nullopt;
        const int along = p.u, across = p.normal;   // (x or y in the plane, the other out of it)
        // The heights where |at - c| is within the span across: t in [ta, tb].
        const double d = std::abs(p.at - c[across]);
        double ta = 0, tb = 1;
        const double h0 = half(across, 0), h1 = half(across, 1);
        if (std::abs(h1 - h0) < 1e-30) {
            if (d > h0 + tol) return std::nullopt;
        } else {
            const double tc = (d - h0) / (h1 - h0);   // where the span across is d
            if (h1 > h0) ta = std::max(ta, tc);
            else tb = std::min(tb, tc);
        }
        if (!(tb > ta + 1e-12)) return std::nullopt;
        const double za = w.first + (w.second - w.first) * ta, zb = w.first + (w.second - w.first) * tb;
        s.kind = Shape::Poly;
        s.points = {QPointF(c[along] - half(along, ta), za), QPointF(c[along] + half(along, ta), za), QPointF(c[along] + half(along, tb), zb),
                    QPointF(c[along] - half(along, tb), zb)};
        return s;
    }
    if (o.kind == QLatin1String("circle")) {
        const double r = o.number(QStringLiteral("radius"), 1e-6);
        const double c[3] = {o.number(QStringLiteral("x"), 0), o.number(QStringLiteral("y"), 0), o.number(QStringLiteral("z"), 0)};
        const std::pair<double, double> w = span(o, QStringLiteral("z"), 1e-6);
        if (o.number(QStringLiteral("rotation 1"), 0) != 0) {
            notes << tr("%1: a circle turned: left out").arg(s.name);
            return std::nullopt;
        }
        if (p.normal == 2) {
            if (p.at < w.first - tol || p.at > w.second + tol) return std::nullopt;
            s.kind = Shape::Circle;
            s.cx = c[0];
            s.cy = c[1];
            s.r = r;
            return s;
        }
        // (A cylinder cut along its axis: a rectangle.)
        const double d = p.at - c[p.normal];
        if (std::abs(d) >= r) return std::nullopt;
        const double half = std::sqrt(r * r - d * d);
        s.kind = Shape::Rect;
        s.x0 = c[p.u] - half;
        s.x1 = c[p.u] + half;
        s.y0 = w.first;
        s.y1 = w.second;
        return s;
    }
    return std::nullopt;
}

/// A polygon clipped to the region's box (Sutherland and Hodgman's).
std::vector<QPointF> clip(std::vector<QPointF> poly, const Plane& p)
{
    auto side = [&](auto inside, auto cross) {
        std::vector<QPointF> out;
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const QPointF a = poly[i], b = poly[(i + 1) % poly.size()];
            const bool ia = inside(a), ib = inside(b);
            if (ia) out.push_back(a);
            if (ia != ib) out.push_back(cross(a, b));
        }
        poly = out;
    };
    auto at = [](QPointF a, QPointF b, double t) { return a + (b - a) * t; };
    if (std::isfinite(p.u0)) side([&](QPointF q) { return q.x() >= p.u0; }, [&](QPointF a, QPointF b) { return at(a, b, (p.u0 - a.x()) / (b.x() - a.x())); });
    if (std::isfinite(p.u1)) side([&](QPointF q) { return q.x() <= p.u1; }, [&](QPointF a, QPointF b) { return at(a, b, (p.u1 - a.x()) / (b.x() - a.x())); });
    if (std::isfinite(p.v0)) side([&](QPointF q) { return q.y() >= p.v0; }, [&](QPointF a, QPointF b) { return at(a, b, (p.v0 - a.y()) / (b.y() - a.y())); });
    if (std::isfinite(p.v1)) side([&](QPointF q) { return q.y() <= p.v1; }, [&](QPointF a, QPointF b) { return at(a, b, (p.v1 - a.y()) / (b.y() - a.y())); });
    // (Points one on another dropped.)
    std::vector<QPointF> out;
    for (const QPointF& q : poly)
        if (out.empty() || std::hypot(q.x() - out.back().x(), q.y() - out.back().y()) > 1e-15) out.push_back(q);
    if (out.size() > 1 && std::hypot(out.front().x() - out.back().x(), out.front().y() - out.back().y()) <= 1e-15) out.pop_back();
    return out;
}

/// The shape kept to the region (none: outside it).
std::optional<Shape> clipped(Shape s, const Plane& p)
{
    if (s.kind == Shape::Rect) {
        s.x0 = std::max(s.x0, p.u0);
        s.x1 = std::min(s.x1, p.u1);
        s.y0 = std::max(s.y0, p.v0);
        s.y1 = std::min(s.y1, p.v1);
        if (!(s.x1 - s.x0 > 1e-15) || !(s.y1 - s.y0 > 1e-15)) return std::nullopt;
        return s;
    }
    if (s.kind == Shape::Circle) {
        if (s.cx - s.r >= p.u0 && s.cx + s.r <= p.u1 && s.cy - s.r >= p.v0 && s.cy + s.r <= p.v1) {
            s.x0 = s.cx - s.r;
            s.x1 = s.cx + s.r;
            s.y0 = s.cy - s.r;
            s.y1 = s.cy + s.r;
            return s;
        }
        s.kind = Shape::Poly;
        for (int i = 0; i < 96; ++i) s.points.emplace_back(s.cx + s.r * std::cos(2 * M_PI * i / 96), s.cy + s.r * std::sin(2 * M_PI * i / 96));
    }
    s.points = clip(s.points, p);
    if (s.points.size() < 3) return std::nullopt;
    double a = 0;
    s.x0 = s.y0 = INFINITY;
    s.x1 = s.y1 = -INFINITY;
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        const QPointF& q = s.points[i];
        const QPointF& r = s.points[(i + 1) % s.points.size()];
        a += q.x() * r.y() - r.x() * q.y();
        s.x0 = std::min(s.x0, q.x());
        s.x1 = std::max(s.x1, q.x());
        s.y0 = std::min(s.y0, q.y());
        s.y1 = std::max(s.y1, q.y());
    }
    if (!(std::abs(a) > 1e-30)) return std::nullopt;
    // (A rectangle's four corners: a rectangle.)
    if (s.points.size() == 4) {
        bool box = true;
        for (const QPointF& q : s.points)
            box = box && (std::abs(q.x() - s.x0) < 1e-15 || std::abs(q.x() - s.x1) < 1e-15) && (std::abs(q.y() - s.y0) < 1e-15 || std::abs(q.y() - s.y1) < 1e-15);
        if (box) s.kind = Shape::Rect;
    }
    return s;
}

/// A label the model may have (letters, digits, _), not one of \a used.
QString label(const QString& name, QSet<QString>& used)
{
    QString out;
    for (const QChar c : name.trimmed()) out += c.isLetterOrNumber() ? c : QLatin1Char('_');
    while (out.contains(QLatin1String("__"))) out.replace(QLatin1String("__"), QLatin1String("_"));
    if (out.endsWith(QLatin1Char('_'))) out.chop(1);
    if (out.isEmpty() || !out[0].isLetter()) out.prepend(QStringLiteral("s"));
    QString l = out;
    for (int k = 2; used.contains(l.toLower()); ++k) l = QStringLiteral("%1_%2").arg(out).arg(k);
    used.insert(l.toLower());
    return l;
}

/// The geometry's objects, in metres scaled to the unit (\a s: metres in
/// one).
QJsonObject shapeJson(const Shape& s, double unit)
{
    if (s.kind == Shape::Rect)
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("rectangle")},
                           {QStringLiteral("label"), s.label},
                           {QStringLiteral("position"), QJsonArray{num(s.x0 / unit), num(s.y0 / unit)}},
                           {QStringLiteral("size"), QJsonArray{num((s.x1 - s.x0) / unit), num((s.y1 - s.y0) / unit)}}};
    if (s.kind == Shape::Circle)
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("circle")},
                           {QStringLiteral("label"), s.label},
                           {QStringLiteral("center"), QJsonArray{num(s.cx / unit), num(s.cy / unit)}},
                           {QStringLiteral("radius"), num(s.r / unit)}};
    QJsonArray pts;
    for (const QPointF& q : s.points) pts.append(QJsonArray{num(q.x() / unit), num(q.y() / unit)});
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("polygon")}, {QStringLiteral("label"), s.label}, {QStringLiteral("points"), pts}};
}

/// A contact's name as its globals have it (semi.I_<name>): letters,
/// digits, _.
QString contactName(const QString& name)
{
    QString out;
    for (const QChar c : name.trimmed()) out += c.isLetterOrNumber() ? c : QLatin1Char('_');
    return out.isEmpty() ? QStringLiteral("c") : out;
}

QJsonObject only(const QStringList& labels)
{
    return QJsonObject{{QStringLiteral("objects"), QJsonArray::fromStringList(labels)}};
}

/// The structures of a scene cut by a plane, kept to its region, in the
/// order they are drawn (the lowest priority first), labelled.
std::vector<Shape> structures(const Scene& scene, const Plane& p, const std::function<int(const Object&)>& order, QSet<QString>& used,
                              QStringList& notes)
{
    std::vector<Shape> out;
    int turn = 0;
    for (const Object& o : scene.objects) {
        if (o.kind != QLatin1String("rect") && o.kind != QLatin1String("poly") && o.kind != QLatin1String("circle") && o.kind != QLatin1String("pyramid"))
            continue;
        if (o.prop(QStringLiteral("enabled")) && o.number(QStringLiteral("enabled"), 1) == 0) continue;
        std::optional<Shape> s = sectionOf(o, p, notes);
        if (!s) continue;
        s = clipped(*s, p);
        if (!s) continue;
        s->order = order(o);
        s->turn = turn++;
        out.push_back(*s);
    }
    // (Drawn with the highest mesh order first: the lower, drawn after, wins.)
    std::stable_sort(out.begin(), out.end(), [](const Shape& a, const Shape& b) { return a.order > b.order; });
    for (Shape& s : out) s.label = label(s.name, used);
    return out;
}

/// The unit of a geometry this size (metres): µm, nm for a well's.
std::pair<QString, double> unitFor(double size)
{
    if (size < 2e-6) return {QStringLiteral("nm"), 1e-9};
    if (size < 2e-3) return {QStringLiteral("um"), 1e-6};
    return {QStringLiteral("mm"), 1e-3};
}

QJsonObject model(const QString& title, const QString& description, const QJsonObject& component, const QJsonArray& studies,
                  const QJsonObject& results, const QJsonArray& parameters = {})
{
    return QJsonObject{{QStringLiteral("format"), QStringLiteral("qucs-s multiphysics 1")},
                       {QStringLiteral("title"), title},
                       {QStringLiteral("description"), description},
                       {QStringLiteral("parameters"), parameters},
                       {QStringLiteral("components"), QJsonArray{component}},
                       {QStringLiteral("studies"), studies},
                       {QStringLiteral("results"), results}};
}

// --- MODE -----------------------------------------------------------------

const Material* materialNamed(const Scene& scene, const QString& name)
{
    for (const Material& m : scene.materials)
        if (m.name == name) return &m;
    return nullptr;
}

/// A material's index at a frequency: an (n,k) material's, a sampled one's
/// (its permittivity between its samples), a dielectric's.
std::optional<std::array<double, 2>> indexAt(const Material& m, double f)
{
    if (m.type == QLatin1String("(n,k) Material") || m.type == QLatin1String("dielectric")) return std::array<double, 2>{m.n[0], m.k[0]};
    if (m.type != QLatin1String("sampled") || m.f.size() < 1 || m.epsRe.size() != m.f.size()) return std::nullopt;
    std::vector<std::size_t> order(m.f.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return m.f[a] < m.f[b]; });
    double re = m.epsRe[order.front()], im = m.epsIm.empty() ? 0 : m.epsIm[order.front()];
    for (std::size_t k = 1; k < order.size(); ++k) {
        const std::size_t a = order[k - 1], b = order[k];
        if (f > m.f[b]) {
            re = m.epsRe[b];
            im = m.epsIm.empty() ? 0 : m.epsIm[b];
            continue;
        }
        const double t = m.f[b] > m.f[a] ? std::clamp((f - m.f[a]) / (m.f[b] - m.f[a]), 0.0, 1.0) : 0.0;
        re = m.epsRe[a] + t * (m.epsRe[b] - m.epsRe[a]);
        im = m.epsIm.empty() ? 0 : m.epsIm[a] + t * (m.epsIm[b] - m.epsIm[a]);
        break;
    }
    const std::complex<double> n = std::sqrt(std::complex<double>(re, std::abs(im)));
    return std::array<double, 2>{n.real(), n.imag()};
}

/// A Sellmeier material's index along an axis at a wavelength (m): n² = c0
/// + c1 λ²/(λ² - c2) + c3 λ²/(λ² - c4) + c5 λ²/(λ² - c6), λ in µm (none
/// where n² is not positive).
std::optional<double> sellmeierAt(const Material& m, int axis, double lambda)
{
    const std::array<double, 7>& c = m.sellmeier[std::size_t(axis)];
    const double l2 = lambda * 1e6 * lambda * 1e6;
    double n2 = c[0];
    for (int k = 1; k < 7; k += 2) {
        if (c[std::size_t(k)] == 0) continue;
        if (std::abs(l2 - c[std::size_t(k + 1)]) < 1e-12) return std::nullopt;
        n2 += c[std::size_t(k)] * l2 / (l2 - c[std::size_t(k + 1)]);
    }
    if (!(n2 > 0)) return std::nullopt;
    return std::sqrt(n2);
}

/// A material's index along x, y and z at a frequency, its κ: an (n,k)
/// material's, a dielectric's, a sampled one's, a Sellmeier one's.
std::optional<std::array<double, 6>> indexOf(const Material& m, double f)
{
    if (m.type == QLatin1String("sellmeier")) {
        std::array<double, 6> out{};
        for (int a = 0; a < 3; ++a) {
            const std::optional<double> n = sellmeierAt(m, a, C0 / f);
            if (!n) return std::nullopt;
            out[std::size_t(a)] = *n;
        }
        return out;
    }
    if (m.type == QLatin1String("(n,k) Material") || m.type == QLatin1String("dielectric"))
        return std::array<double, 6>{m.n[0], m.n[1], m.n[2], m.k[0], m.k[1], m.k[2]};
    const std::optional<std::array<double, 2>> nk = indexAt(m, f);
    if (!nk) return std::nullopt;
    return std::array<double, 6>{(*nk)[0], (*nk)[0], (*nk)[0], (*nk)[1], (*nk)[1], (*nk)[1]};
}

/// A sampled or Sellmeier material's index of the wavelength (lambda0), as
/// texts: n along x, y and z, κ (empty: none), where it holds, and what it is.
struct Dispersion {
    QString n[3], kappa, range, what;
};

std::optional<Dispersion> dispersionOf(const Material& m)
{
    Dispersion d;
    if (m.type == QLatin1String("sellmeier")) {
        // n² = c0 + c1 λ²/(λ² - c2) + c3 λ²/(λ² - c4) + c5 λ²/(λ² - c6), λ in µm.
        for (int a = 0; a < 3; ++a) {
            const auto& c = m.sellmeier[std::size_t(a)];
            QString t = QStringLiteral("sqrt(%1").arg(num(c[0]));
            for (int k = 1; k < 7; k += 2)
                if (c[std::size_t(k)] != 0)
                    t += QStringLiteral(" + %1*lambda0^2/(lambda0^2 - %2[um^2])").arg(num(c[std::size_t(k)]), num(c[std::size_t(k + 1)]));
            d.n[a] = t + QLatin1Char(')');
        }
        d.what = QObject::tr("Sellmeier coefficients");
        return d;
    }
    if (m.type != QLatin1String("sampled") || m.f.empty() || m.epsRe.size() != m.f.size() || m.epsIm.size() != m.f.size()) return std::nullopt;
    // (λ, n, κ) at each point from 0.2 to 20 µm, λ rising; at most 150 of them.
    std::vector<std::array<double, 3>> rows;
    for (std::size_t k = 0; k < m.f.size(); ++k) {
        if (!(m.f[k] > 0)) continue;
        const double lambda = C0 / m.f[k];
        if (lambda < 0.2e-6 || lambda > 20e-6) continue;
        const std::complex<double> nk = std::sqrt(std::complex<double>(m.epsRe[k], m.epsIm[k]));
        rows.push_back({lambda, nk.real(), std::abs(nk.imag())});
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a[0] == b[0]; }), rows.end());
    if (rows.size() < 2) return std::nullopt;
    if (rows.size() > 150) {
        std::vector<std::array<double, 3>> kept;
        for (std::size_t k = 0; k < 150; ++k) kept.push_back(rows[k * (rows.size() - 1) / 149]);
        rows = kept;
    }
    QString n = QStringLiteral("interp(lambda0"), kappa = QStringLiteral("interp(lambda0");
    bool absorbs = false;
    for (const auto& [lambda, re, im] : rows) {
        const QString at = QStringLiteral("%1[um]").arg(formatNumber(lambda * 1e6, 6));
        n += QStringLiteral(", %1, %2").arg(at, formatNumber(re, 6));
        kappa += QStringLiteral(", %1, %2").arg(at, formatNumber(im, 6));
        absorbs = absorbs || im > 0;
    }
    d.n[0] = d.n[1] = d.n[2] = n + QLatin1Char(')');
    if (absorbs) d.kappa = kappa + QLatin1Char(')');
    d.range = QStringLiteral("%1[um] %2[um]").arg(formatNumber(rows.front()[0] * 1e6, 6), formatNumber(rows.back()[0] * 1e6, 6));
    d.what = QObject::tr("data, %1 points from %2 to %3 µm").arg(rows.size()).arg(formatNumber(rows.front()[0] * 1e6, 4), formatNumber(rows.back()[0] * 1e6, 4));
    return d;
}

LumericalImport buildMode(const Scene& scene, const Object& fde)
{
    LumericalImport out;
    QStringList notes;
    // Its plane and region.
    const QString type = fde.text(QStringLiteral("solver type")).toLower();
    Plane p;
    if (type.contains(QLatin1String("x normal"))) {
        p.normal = 0;
        p.u = 1;
        p.v = 2;
    } else if (type.contains(QLatin1String("y normal"))) {
        p.normal = 1;
        p.u = 0;
        p.v = 2;
    }
    const QLatin1String axes[3] = {QLatin1String("x"), QLatin1String("y"), QLatin1String("z")};
    std::pair<double, double> b[3];
    for (int a = 0; a < 3; ++a) b[a] = span(fde, axes[a], a == p.normal ? 0 : 2e-6);
    p.at = (b[p.normal].first + b[p.normal].second) / 2;
    p.u0 = b[p.u].first;
    p.u1 = b[p.u].second;
    p.v0 = b[p.v].first;
    p.v1 = b[p.v].second;
    if (!(p.u1 > p.u0) || !(p.v1 > p.v0)) {
        out.error = tr("its FDE region has no extent");
        return out;
    }
    // The wavelength, the modes, the index about.
    auto analysis = [&](const char* key) { return fde.prop(QStringLiteral("analysis/") + QLatin1String(key)); };
    double lambda = 1.55e-6;
    if (const Value* v = analysis("wavelength")) lambda = v->toNumber(lambda);
    else if (const Value* f = analysis("frequency")) lambda = C0 / f->toNumber(C0 / lambda);
    const int askedModes = analysis("number of trial modes") ? whole(analysis("number of trial modes")->toNumber(4), 1, 1000, -1) : 4;
    if (askedModes < 0) notes << tr("its number of trial modes %1: no more than 1,000 - 4 taken").arg(num(analysis("number of trial modes")->toNumber(4)));
    const int modes = askedModes < 0 ? 4 : askedModes;
    QString neff;
    if (analysis("search") && analysis("search")->toText().contains(QLatin1String("near")) && analysis("n")) neff = num(analysis("n")->toNumber(1));
    else if (analysis("use max index") && analysis("use max index")->toNumber(1) == 0 && analysis("n")) neff = num(analysis("n")->toNumber(1));
    // The structures, their materials.
    QSet<QString> used = {QStringLiteral("window")};
    auto order = [&](const Object& o) {
        if (o.number(QStringLiteral("override mesh order from material database"), 0) != 0) return whole(o.number(QStringLiteral("mesh order"), 2), -1000000000, 1000000000, 2);
        if (const Material* m = materialNamed(scene, o.text(QStringLiteral("material")))) return m->priority;
        return 2;
    };
    const std::vector<Shape> shapes = structures(scene, p, order, used, notes);
    if (shapes.empty()) {
        out.error = tr("no structure crosses its FDE region");
        return out;
    }
    const double size = std::max(p.u1 - p.u0, p.v1 - p.v0);
    const std::pair<QString, double> unitScale = unitFor(size);
    const QString unit = unitScale.first;
    const double scale = unitScale.second;
    QJsonArray geometry, materials, features;
    // (The region's background first: its index, else air.)
    geometry.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("rectangle")},
                                {QStringLiteral("label"), QStringLiteral("window")},
                                {QStringLiteral("position"), QJsonArray{num(p.u0 / scale), num(p.v0 / scale)}},
                                {QStringLiteral("size"), QJsonArray{num((p.u1 - p.u0) / scale), num((p.v1 - p.v0) / scale)}}});
    const double background = fde.prop(QStringLiteral("background index")) ? fde.number(QStringLiteral("background index"), 1) : 1.0;
    materials.append(QJsonObject{{QStringLiteral("label"), QStringLiteral("background")},
                                 {QStringLiteral("n"), num(background)},
                                 {QStringLiteral("selection"), only({QStringLiteral("window")})}});
    std::map<QString, QJsonObject> made;
    QStringList saidLibrary;
    auto say = [&](const QString& note) {
        if (!notes.contains(note)) notes << note;
    };
    // (An index along x, y, z: a wave feature of its own, diagonal where
    // they differ - the material its x one, for the mesh.)
    auto indexed = [&features](const Shape& s, const QString& label, const std::array<double, 6>& nk, QJsonObject feature) {
        const bool diagonal = std::abs(nk[0] - nk[1]) > 1e-12 || std::abs(nk[0] - nk[2]) > 1e-12;
        if (!diagonal && feature.isEmpty())
            return QJsonObject{{QStringLiteral("label"), label}, {QStringLiteral("n"), num(nk[0])}, {QStringLiteral("kappa"), num(nk[3])}};
        feature.insert(QStringLiteral("type"), QStringLiteral("wave"));
        feature.insert(QStringLiteral("label"), label);
        feature.insert(QStringLiteral("source"), QStringLiteral("user"));
        if (diagonal) {
            feature.insert(QStringLiteral("anisotropy"), QStringLiteral("diagonal"));
            feature.insert(QStringLiteral("nx"), num(nk[0]));
            feature.insert(QStringLiteral("ny"), num(nk[1]));
            feature.insert(QStringLiteral("nz"), num(nk[2]));
        } else
            feature.insert(QStringLiteral("n"), num(nk[0]));
        // (κ along x, y and z where they differ: a dichroic one's.)
        const bool dichroic = std::abs(nk[3] - nk[4]) > 1e-12 || std::abs(nk[3] - nk[5]) > 1e-12;
        feature.insert(QStringLiteral("kappa"), dichroic ? QStringLiteral("%1; %2; %3").arg(num(nk[3]), num(nk[4]), num(nk[5])) : num(nk[3]));
        feature.insert(QStringLiteral("selection"), only({s.label}));
        features.append(feature);
        return QJsonObject{{QStringLiteral("label"), label}, {QStringLiteral("n"), num(nk[0])}};
    };
    for (const Shape& s : shapes) {
        geometry.append(shapeJson(s, scale));
        QJsonObject m;
        const QString mat = s.material.trimmed();
        if (mat.isEmpty() || mat.startsWith(QLatin1Char('<'))) {
            // An object defined dielectric: its own index - one, or along
            // x, y and z ("nx;ny;nz").
            const QStringList parts = s.index.split(QLatin1Char(';'), Qt::SkipEmptyParts);
            std::array<double, 6> nk{1, 1, 1, 0, 0, 0};
            bool ok = parts.size() == 1 || parts.size() == 3;
            for (int a = 0; a < 3 && ok; ++a) nk[std::size_t(a)] = parts[parts.size() == 3 ? a : 0].trimmed().toDouble(&ok);
            if (!ok) {
                nk = {1, 1, 1, 0, 0, 0};
                notes << tr("%1: its index %2 not a number (nor three): 1 taken").arg(s.name, s.index);
            }
            m = indexed(s, tr("%1's index").arg(s.name), nk, {});
            m.remove(QStringLiteral("kappa"));
        } else if (const Material* lm = materialNamed(scene, mat)) {
            const Material* base = lm->type == QLatin1String("thermal") ? materialNamed(scene, lm->base) : nullptr;
            const std::optional<std::array<double, 6>> nk = indexOf(base ? *base : *lm, C0 / lambda);
            if (lm->type == QLatin1String("PEC")) {
                m = QJsonObject{{QStringLiteral("label"), mat}, {QStringLiteral("n"), QStringLiteral("1")}, {QStringLiteral("kappa"), QStringLiteral("1e4")}};
                notes << tr("%1: a perfect conductor - an index with κ 1e4 (a PEC boundary on its edges is the exact one)").arg(mat);
            } else if (lm->type == QLatin1String("thermal") && nk) {
                // Temperature dependent: its material's index, and dn/dT (T
                // - Tref) at the FDE's temperature.
                const double T = fde.number(QStringLiteral("simulation temperature"), 300);
                QJsonObject feature;
                if (lm->thermal) {
                    // (Along x, y and z where they differ.)
                    const bool along = std::abs(lm->dndT[0] - lm->dndT[1]) > 1e-15 || std::abs(lm->dndT[0] - lm->dndT[2]) > 1e-15;
                    const QString dndT = along ? QStringLiteral("%1[1/K]; %2[1/K]; %3[1/K]").arg(num(lm->dndT[0]), num(lm->dndT[1]), num(lm->dndT[2]))
                                               : num(lm->dndT[0]) + QStringLiteral("[1/K]");
                    feature = QJsonObject{{QStringLiteral("thermooptic"), true},
                                          {QStringLiteral("dndT"), dndT},
                                          {QStringLiteral("T"), num(T) + QStringLiteral("[K]")},
                                          {QStringLiteral("Tref"), num(lm->tref) + QStringLiteral("[K]")}};
                    say(tr("%1: %2's index at %3 nm (n %4) and dn/dT %5 /K from %6 K, at the FDE's %7 K - its T a heat physics' ht.T to tune it")
                            .arg(mat, lm->base, num(lambda * 1e9), QString::number((*nk)[0], 'f', 4), num(lm->dndT[0]), num(lm->tref), num(T)));
                    if (lm->dkdT != 0) say(tr("%1: its dκ/dT %2 /K not taken").arg(mat, num(lm->dkdT)));
                } else
                    say(tr("%1: %2's index at %3 nm (its temperature effects off)").arg(mat, lm->base, num(lambda * 1e9)));
                if (lm->carriers)
                    say(tr("%1: its free carriers' terms (Soref's) not taken - no carriers here (the wave feature's plasma dispersion, with a "
                           "semiconductor physics' semi.n and semi.p)")
                            .arg(mat));
                m = indexed(s, mat, *nk, feature);
            } else if (nk && lm->type != QLatin1String("thermal")) {
                m = indexed(s, mat, *nk, {});
                if (const std::optional<Dispersion> dn = dispersionOf(*lm)) {
                    // Of the wavelength, as Lumerical has it: its data's
                    // points (interp, at most 150 from 0.2 to 20 µm), its
                    // Sellmeier coefficients - along x, y and z where they differ.
                    const bool diagonal = dn->n[1] != dn->n[0] || dn->n[2] != dn->n[0];
                    if (diagonal) {
                        QJsonObject feature{{QStringLiteral("type"), QStringLiteral("wave")}, {QStringLiteral("label"), mat},
                                            {QStringLiteral("source"), QStringLiteral("user")}, {QStringLiteral("anisotropy"), QStringLiteral("diagonal")},
                                            {QStringLiteral("nx"), dn->n[0]}, {QStringLiteral("ny"), dn->n[1]}, {QStringLiteral("nz"), dn->n[2]},
                                            {QStringLiteral("kappa"), dn->kappa.isEmpty() ? QStringLiteral("0") : dn->kappa},
                                            {QStringLiteral("selection"), only({s.label})}};
                        features.append(feature);
                        m = QJsonObject{{QStringLiteral("label"), mat}, {QStringLiteral("n"), dn->n[0]}};
                    } else {
                        m = QJsonObject{{QStringLiteral("label"), mat}, {QStringLiteral("n"), dn->n[0]}};
                        if (!dn->kappa.isEmpty()) m.insert(QStringLiteral("kappa"), dn->kappa);
                    }
                    if (!dn->range.isEmpty()) m.insert(QStringLiteral("nrange"), dn->range);
                    say(tr("%1: its index of the wavelength, the Lumerical %2 (n %3 κ %4 at %5 nm)")
                            .arg(mat, dn->what, QString::number((*nk)[0], 'f', 4), QString::number((*nk)[3], 'g', 3), num(lambda * 1e9)));
                }
            } else {
                m = QJsonObject{{QStringLiteral("label"), mat}, {QStringLiteral("n"), QStringLiteral("1")}};
                if (lm->type == QLatin1String("thermal"))
                    notes << tr("%1: a temperature dependent material whose material (%2) is not read: n = 1 - set its index").arg(mat, lm->base);
                else
                    notes << tr("%1: a material of a kind not read (%2): n = 1 - set its index").arg(mat, lm->type);
            }
        } else if (const QString ours = libraryOf(mat, false); !ours.isEmpty()) {
            m = QJsonObject{{QStringLiteral("material"), ours}};
            if (!saidLibrary.contains(mat)) saidLibrary << tr("%1 as %2").arg(mat, ours);
        } else {
            m = QJsonObject{{QStringLiteral("label"), mat}, {QStringLiteral("n"), QStringLiteral("1")}};
            notes << tr("%1: no such material in the library: n = 1 - set its index").arg(mat);
        }
        m.insert(QStringLiteral("selection"), only({s.label}));
        materials.append(m);
    }
    if (!saidLibrary.isEmpty()) notes << tr("Lumerical materials as the library's: %1").arg(saidLibrary.join(QStringLiteral(", ")));
    // (The guiding structures - the highest index's - meshed to an eighth
    // of their smaller side.)
    QJsonArray meshFeatures;
    {
        double nMax = 0;
        std::vector<double> nOf;
        for (const QJsonValue& v : materials) {
            const QJsonObject m = v.toObject();
            nOf.push_back(m.contains(QStringLiteral("n")) ? m.value(QStringLiteral("n")).toString().toDouble() : 0);
        }
        // (materials[0] the window's; then one for each shape.)
        for (std::size_t i = 1; i < nOf.size(); ++i) nMax = std::max(nMax, nOf[i]);
        for (std::size_t i = 1; i < nOf.size() && i - 1 < shapes.size(); ++i) {
            if (nMax <= 0 || nOf[i] < nMax - 1e-9) continue;
            const Shape& s = shapes[i - 1];
            const double h = std::min(s.x1 - s.x0, s.y1 - s.y0) / 8;
            if (h < 0.05 * size)
                meshFeatures.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("size")},
                                                {QStringLiteral("level"), QStringLiteral("domain")},
                                                {QStringLiteral("selection"), only({s.label})},
                                                {QStringLiteral("hmax"), num(h / scale)}});
        }
    }
    // Its sides: PML (a frame of PML domains inside the region, 12 of its
    // mesh steps, else 10 % of it), PMC, else metal - the physics' own.
    const QLatin1String bcKeys[4] = {QLatin1String("x min bc"), QLatin1String("x max bc"), QLatin1String("y min bc"), QLatin1String("y max bc")};
    QString bc[4];
    for (int k = 0; k < 4; ++k) {
        const QString key = p.normal == 2 ? bcKeys[k] : QString(bcKeys[k]).replace(0, 1, k < 2 ? axes[p.u] : axes[p.v]);
        bc[k] = fde.text(key).trimmed().toLower();
    }
    const double meshStep = std::max(fde.number(QStringLiteral("dx"), NAN), fde.number(QStringLiteral("dy"), NAN));
    QStringList frame;
    for (int k = 0; k < 4; ++k) {
        if (bc[k] == QLatin1String("pml")) {
            const double t = std::isfinite(meshStep) ? std::min(12 * meshStep, 0.25 * (k < 2 ? p.u1 - p.u0 : p.v1 - p.v0)) : 0.1 * (k < 2 ? p.u1 - p.u0 : p.v1 - p.v0);
            const double x0 = k == 1 ? p.u1 - t : p.u0, x1 = k == 0 ? p.u0 + t : p.u1;
            const double y0 = k == 3 ? p.v1 - t : p.v0, y1 = k == 2 ? p.v0 + t : p.v1;
            const QString l = label(QStringLiteral("pml_%1").arg(QStringList{QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("bottom"), QStringLiteral("top")}[k]), used);
            geometry.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("rectangle")},
                                        {QStringLiteral("label"), l},
                                        {QStringLiteral("position"), QJsonArray{num(x0 / scale), num(y0 / scale)}},
                                        {QStringLiteral("size"), QJsonArray{num((x1 - x0) / scale), num((y1 - y0) / scale)}}});
            frame << l;
        } else if (bc[k] == QLatin1String("pmc")) {
            const double e = 1e-6 * size;
            const double x0 = k == 1 ? p.u1 - e : p.u0 - e, x1 = k == 0 ? p.u0 + e : p.u1 + e;
            const double y0 = k == 3 ? p.v1 - e : p.v0 - e, y1 = k == 2 ? p.v0 + e : p.v1 + e;
            features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("pmc")},
                                        {QStringLiteral("label"), tr("PMC (%1)").arg(bcKeys[k])},
                                        {QStringLiteral("selection"), QJsonObject{{QStringLiteral("box"), QJsonArray{num(x0 / scale), num(y0 / scale), num(x1 / scale), num(y1 / scale)}}}}});
        } else if (!bc[k].isEmpty() && bc[k] != QLatin1String("metal")) {
            notes << tr("its %1 %2: metal taken").arg(bcKeys[k], bc[k]);
        }
    }
    if (!frame.isEmpty()) {
        features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("pml")}, {QStringLiteral("label"), tr("Absorbing layers")}, {QStringLiteral("selection"), only(frame)}});
        notes << tr("its PML sides: layers %1 thick inside the region").arg(std::isfinite(meshStep) ? tr("of 12 of its mesh steps") : tr("of a tenth of it"));
    }
    geometry.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("form union")}});
    // The study: the wavelength, a sweep from it.
    QString frequency = QStringLiteral("%1[nm]").arg(num(lambda * 1e9));
    if (const Value* stop = analysis("stop wavelength"); stop && analysis("number of points")) {
        const int n = whole(analysis("number of points")->toNumber(1), 1, 10000, -1);
        if (n < 0) notes << tr("its wavelength sweep of %1 points: no more than 10,000 - its wavelength alone").arg(num(analysis("number of points")->toNumber(1)));
        if (n > 1) frequency = QStringLiteral("linspace(%1[Hz], %2[Hz], %3)").arg(num(C0 / lambda), num(C0 / stop->toNumber(lambda))).arg(n);
    }
    QJsonObject step{{QStringLiteral("type"), QStringLiteral("mode")}, {QStringLiteral("frequency"), frequency}, {QStringLiteral("modes"), std::max(1, modes)}};
    if (!neff.isEmpty()) step.insert(QStringLiteral("neff"), neff);
    const QJsonObject component{{QStringLiteral("unit"), unit},
                                {QStringLiteral("geometry"), geometry},
                                {QStringLiteral("materials"), materials},
                                {QStringLiteral("physics"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("modes")}, {QStringLiteral("tag"), QStringLiteral("emw")}, {QStringLiteral("features"), features}}}},
                                {QStringLiteral("mesh"), meshFeatures.isEmpty() ? QJsonObject{{QStringLiteral("size"), QStringLiteral("fine")}}
                                                                                : QJsonObject{{QStringLiteral("size"), QStringLiteral("fine")}, {QStringLiteral("features"), meshFeatures}}}};
    const QJsonObject results{
        {QStringLiteral("plots"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("plotgroup")}, {QStringLiteral("label"), tr("Electric Field")}, {QStringLiteral("mode"), 1},
                                                         {QStringLiteral("plots"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("surface")}, {QStringLiteral("expression"), QStringLiteral("emw.normE")}}}}}}},
        {QStringLiteral("derived"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("global")}, {QStringLiteral("label"), tr("Effective index and loss")},
                                                           {QStringLiteral("expression"), QStringLiteral("emw.neff; emw.loss")}}}}};
    out.model = model(tr("%1 (Lumerical MODE)").arg(scene.source), tr("Imported from %1: its FDE region's cross-section, %2 structures.").arg(scene.source).arg(shapes.size()),
                      component, QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("study")}, {QStringLiteral("label"), tr("Modes")}, {QStringLiteral("steps"), QJsonArray{step}}}},
                      results);
    out.notes = QStringList{tr("%1: an FDE region %2 x %3 µm (%4), %5 structures, %6 modes at %7 nm")
                                .arg(scene.source, num((p.u1 - p.u0) * 1e6), num((p.v1 - p.v0) * 1e6),
                                     p.normal == 2 ? QStringLiteral("2D Z normal") : p.normal == 1 ? QStringLiteral("2D Y normal") : QStringLiteral("2D X normal"))
                                .arg(shapes.size())
                                .arg(modes)
                                .arg(num(lambda * 1e9))}
                + notes + scene.notes;
    return out;
}

// --- CHARGE ---------------------------------------------------------------

/// What a semiconductor's Lumerical models make here at temperature \a T:
/// its material's properties - Eg (Varshni's), Nc and Nv of its masses,
/// χ from its work function (Lumerical's to the intrinsic level: W - Eg/2
/// - kT/2 ln(Nc/Nv)), εr, the lattice's mobilities, the SRH lifetimes -
/// and its features: the material model's (its mobility by doping -
/// Masetti's, Caughey and Thomas's -, saturation, band gap narrowing,
/// Fermi-Dirac's statistics), SRH's (doping dependence, trap-assisted
/// tunnelling, its trap level), Auger's, radiative, impact ionization
/// (Selberherr's), band-to-band tunnelling (Hurkx's) - their selections
/// left to set.
struct SemiconductorModels {
    bool ok = false;
    QJsonObject material, model;
    QJsonArray features;
    QStringList notes;
};

SemiconductorModels semiconductorModels(const Object& o, double T, bool fermi)
{
    SemiconductorModels out;
    const Value* all = o.prop(QStringLiteral("@models"));
    if (!all || all->kind != Value::Struct) return out;
    const QString name = o.name();
    auto model = [&](const QString& key) { return all->field(key); };
    auto on = [&](const QString& key) {
        const Value* m = model(key);
        const Value* e = m ? m->field(QStringLiteral("enabled")) : nullptr;
        return m && (!e || e->toNumber(1) != 0);
    };
    auto active = [&](const QString& key) {
        const Value* m = model(key);
        const Value* a = m ? m->field(QStringLiteral("active")) : nullptr;
        return a ? a->toText() : QString();
    };
    auto param = [&](const QString& key, const QString& p, double otherwise = NAN) {
        const Value* m = model(key);
        const Value* ps = m ? m->field(QStringLiteral("params")) : nullptr;
        const Value* v = ps ? ps->field(p) : nullptr;
        return v ? v->toNumber(otherwise) : otherwise;
    };
    // (A parameter at 300 K and its exponent: X (T/300)^η.)
    auto warm = [&](const QString& key, const QString& p300, const QString& eta) {
        const double x = param(key, p300), e = param(key, eta, 0);
        return x * std::pow(T / 300, std::isfinite(e) ? e : 0.0);
    };
    // A constant's A0, or the universal temperature's A (T/300)^η.
    auto constantOrUniversal = [&](const QString& key) {
        const QString a = active(key);
        if (a == QLatin1String("Constant")) return param(key, QStringLiteral("A0"));
        if (a.startsWith(QLatin1String("Universal"))) return param(key, QStringLiteral("A")) * std::pow(T / 300, param(key, QStringLiteral("eta"), 0));
        return double(NAN);
    };
    const double W = o.number(QStringLiteral("work function"), NAN);
    // Its band gap and masses.
    double Eg = NAN;
    if (active(QStringLiteral("bandgap")) == QLatin1String("Constant")) Eg = param(QStringLiteral("bandgap"), QStringLiteral("A0"));
    else if (model(QStringLiteral("bandgap")))
        Eg = param(QStringLiteral("bandgap"), QStringLiteral("y0")) - param(QStringLiteral("bandgap"), QStringLiteral("alpha"), 0) * T * T / (T + param(QStringLiteral("bandgap"), QStringLiteral("beta"), 0));
    auto mass = [&](const QString& key) {
        if (active(key) == QLatin1String("Constant")) return param(key, QStringLiteral("A0"));
        return param(key, QStringLiteral("y0")) + param(key, QStringLiteral("alpha"), 0) * T + param(key, QStringLiteral("beta"), 0) * T * T;
    };
    const double me = mass(QStringLiteral("mass n")), mh = mass(QStringLiteral("mass p"));
    if (!(Eg > 0) || !(me > 0) || !(mh > 0) || !std::isfinite(W)) return out;
    const double Nc = 2.509e19 * std::pow(me, 1.5) * std::pow(T / 300, 1.5), Nv = 2.509e19 * std::pow(mh, 1.5) * std::pow(T / 300, 1.5);   // cm⁻³
    const double kT = 8.617333262e-5 * T;
    const double chi = W - Eg / 2 - kT / 2 * std::log(Nc / Nv);
    out.material = QJsonObject{{QStringLiteral("label"), name},
                               {QStringLiteral("Eg"), QStringLiteral("%1[eV]").arg(num(Eg))},
                               {QStringLiteral("chi"), QStringLiteral("%1[eV]").arg(num(chi))},
                               {QStringLiteral("Nc"), QStringLiteral("%1[1/cm^3]").arg(num(Nc))},
                               {QStringLiteral("Nv"), QStringLiteral("%1[1/cm^3]").arg(num(Nv))}};
    if (o.prop(QStringLiteral("@epsilonr"))) out.material.insert(QStringLiteral("epsilonr"), num(o.number(QStringLiteral("@epsilonr"))));
    out.notes << tr("%1: the Lumerical models at %2 K - Eg %3 eV (%4), the masses' Nc %5, Nv %6 cm⁻³, χ %7 eV from its work function "
                    "%8 eV (to the intrinsic level)")
                     .arg(name, num(T), num(Eg), active(QStringLiteral("bandgap")) == QLatin1String("Constant") ? tr("constant") : tr("Varshni's"), num(Nc), num(Nv),
                          num(chi), num(W));
    // The lattice's mobilities, their temperature exponent.
    QJsonObject m{{QStringLiteral("type"), QStringLiteral("model")}, {QStringLiteral("label"), tr("%1 (Lumerical models)").arg(name)}};
    for (const char* c : {"n", "p"}) {
        const QString key = QStringLiteral("mobility lattice %1").arg(QLatin1String(c));
        const double mu = constantOrUniversal(key);
        if (std::isfinite(mu)) out.material.insert(QStringLiteral("mu%1").arg(QLatin1String(c)), QStringLiteral("%1[cm^2/(V*s)]").arg(num(mu)));
        if (active(key).startsWith(QLatin1String("Universal"))) m.insert(QStringLiteral("zeta%1").arg(QLatin1String(c)), num(-param(key, QStringLiteral("eta"), 0)));
    }
    // By doping.
    const QString impurity = on(QStringLiteral("mobility impurity n")) ? active(QStringLiteral("mobility impurity n")) : QString();
    if (impurity == QLatin1String("Masetti")) {
        m.insert(QStringLiteral("mobility"), QStringLiteral("masetti"));
        for (const char* c : {"n", "p"}) {
            const QString key = QStringLiteral("mobility impurity %1").arg(QLatin1String(c));
            const QString C = QLatin1String(c);
            const double muMin = warm(key, QStringLiteral("muMin(300)"), QStringLiteral("eta_muMin"));
            m.insert(QStringLiteral("m1") + C, QStringLiteral("%1[cm^2/(V*s)]").arg(num(muMin)));
            m.insert(QStringLiteral("m2") + C, QStringLiteral("%1[cm^2/(V*s)]").arg(num(muMin)));
            m.insert(QStringLiteral("mu1") + C, QStringLiteral("%1[cm^2/(V*s)]").arg(num(warm(key, QStringLiteral("mu2(300)"), QStringLiteral("eta_mu2")))));
            m.insert(QStringLiteral("pc") + C, QStringLiteral("0[1/cm^3]"));
            m.insert(QStringLiteral("cr") + C, QStringLiteral("%1[1/cm^3]").arg(num(warm(key, QStringLiteral("Cr(300)"), QStringLiteral("eta_Cr")))));
            m.insert(QStringLiteral("cs") + C, QStringLiteral("%1[1/cm^3]").arg(num(warm(key, QStringLiteral("Cs(300)"), QStringLiteral("eta_Cs")))));
            m.insert(QStringLiteral("ma") + C, num(warm(key, QStringLiteral("alpha(300)"), QStringLiteral("eta_alpha"))));
            m.insert(QStringLiteral("mb") + C, num(warm(key, QStringLiteral("beta(300)"), QStringLiteral("eta_beta"))));
        }
    } else if (impurity == QLatin1String("Caughey-Thomas")) {
        m.insert(QStringLiteral("mobility"), QStringLiteral("doping"));
        for (const char* c : {"n", "p"}) {
            const QString key = QStringLiteral("mobility impurity %1").arg(QLatin1String(c));
            const QString C = QLatin1String(c);
            m.insert(QStringLiteral("mu") + C + QStringLiteral("min"), QStringLiteral("%1[cm^2/(V*s)]").arg(num(warm(key, QStringLiteral("muMin300"), QStringLiteral("eta1")))));
            m.insert(QStringLiteral("nref") + C, QStringLiteral("%1[1/cm^3]").arg(num(warm(key, QStringLiteral("N300"), QStringLiteral("eta2")))));
            m.insert(QStringLiteral("alpha") + C, num(warm(key, QStringLiteral("p300"), QStringLiteral("eta3"))));
        }
    } else if (!impurity.isEmpty())
        out.notes << tr("%1: the Lumerical %2 mobility not here: its lattice's alone").arg(name, impurity);
    if (!impurity.isEmpty() && on(QStringLiteral("mobility impurity p")) && active(QStringLiteral("mobility impurity p")) != impurity)
        out.notes << tr("%1: its holes' mobility by doping the Lumerical %2, here as its electrons' (%3)").arg(name, active(QStringLiteral("mobility impurity p")), impurity);
    // Saturation (Caughey and Thomas's high field).
    if (on(QStringLiteral("high field n")) || on(QStringLiteral("high field p"))) {
        m.insert(QStringLiteral("saturation"), true);
        for (const char* c : {"n", "p"}) {
            const QString C = QLatin1String(c);
            const QString vkey = QStringLiteral("vsat ") + C;
            const double v = active(vkey) == QLatin1String("Constant") ? param(vkey, QStringLiteral("A0")) : param(vkey, QStringLiteral("vsat300"));
            if (std::isfinite(v)) m.insert(QStringLiteral("vsat") + C, QStringLiteral("%1[cm/s]").arg(num(v)));
            // (Of the temperature: Quay's A, its Av; Constant, none.)
            const double A = active(vkey) == QLatin1String("Constant") ? 0.0 : param(vkey, QStringLiteral("Av"));
            m.insert(QStringLiteral("Av") + C, num(std::isfinite(A) ? A : 0.0));
            const double beta = param(QStringLiteral("high field ") + C, QStringLiteral("beta"));
            if (beta > 0) m.insert(QStringLiteral("beta") + C, num(beta));
        }
        // (Its driving field: 1 the quasi-Fermi potential's gradient, 0 the
        // field - as the projects' scripts set them; inferred.)
        const Value* hf = model(QStringLiteral("high field n"));
        const Value* df = hf ? hf->field(QStringLiteral("driving")) : nullptr;
        m.insert(QStringLiteral("driving"), df && df->toNumber() == 0 ? QStringLiteral("field") : QStringLiteral("current"));
    }
    if (on(QStringLiteral("bgn n")) && active(QStringLiteral("bgn n")) == QLatin1String("Slotboom") && param(QStringLiteral("bgn n"), QStringLiteral("V1"), 0) > 0) {
        m.insert(QStringLiteral("bgn"), true);
        m.insert(QStringLiteral("Ebgn"), QStringLiteral("%1[eV]").arg(num(param(QStringLiteral("bgn n"), QStringLiteral("V1")))));
        m.insert(QStringLiteral("Nbgn"), QStringLiteral("%1[1/cm^3]").arg(num(param(QStringLiteral("bgn n"), QStringLiteral("N0")))));
        m.insert(QStringLiteral("Cbgn"), num(param(QStringLiteral("bgn n"), QStringLiteral("C"), 0.5)));
    }
    if (fermi) m.insert(QStringLiteral("fermi"), true);
    out.model = m;
    // Recombination: SRH (its lifetimes on the material), Auger, radiative.
    const double taun = constantOrUniversal(QStringLiteral("srh tau n")), taup = constantOrUniversal(QStringLiteral("srh tau p"));
    if (on(QStringLiteral("srh tau n")) && taun > 0 && taup > 0) {
        out.material.insert(QStringLiteral("taun"), QStringLiteral("%1[s]").arg(num(taun)));
        out.material.insert(QStringLiteral("taup"), QStringLiteral("%1[s]").arg(num(taup)));
        QJsonObject srh{{QStringLiteral("type"), QStringLiteral("srh")}, {QStringLiteral("label"), tr("%1: SRH").arg(name)}};
        const double Et = param(QStringLiteral("trap n"), QStringLiteral("A0"), 0);
        srh.insert(QStringLiteral("Et"), QStringLiteral("%1[eV]").arg(num(Et)));
        if (on(QStringLiteral("srh doping n")) && active(QStringLiteral("srh doping n")) == QLatin1String("Fossum")) {
            srh.insert(QStringLiteral("dependence"), QStringLiteral("doping"));
            srh.insert(QStringLiteral("N0n"), QStringLiteral("%1[1/cm^3]").arg(num(param(QStringLiteral("srh doping n"), QStringLiteral("N0")))));
            srh.insert(QStringLiteral("N0p"), QStringLiteral("%1[1/cm^3]").arg(num(param(QStringLiteral("srh doping p"), QStringLiteral("N0"), param(QStringLiteral("srh doping n"), QStringLiteral("N0"))))));
            srh.insert(QStringLiteral("fA"), num(param(QStringLiteral("srh doping n"), QStringLiteral("alpha"), 1)));
            srh.insert(QStringLiteral("fB"), num(param(QStringLiteral("srh doping n"), QStringLiteral("beta"), 0)));
            srh.insert(QStringLiteral("fC"), num(param(QStringLiteral("srh doping n"), QStringLiteral("gamma"), 1)));
            srh.insert(QStringLiteral("fE"), num(param(QStringLiteral("srh doping n"), QStringLiteral("sigma"), 1)));
        } else if (on(QStringLiteral("srh doping n")) && !active(QStringLiteral("srh doping n")).isEmpty())
            out.notes << tr("%1: the Lumerical %2 doping dependence of its lifetimes not here: none").arg(name, active(QStringLiteral("srh doping n")));
        if (on(QStringLiteral("srh field n")) && active(QStringLiteral("srh field n")) == QLatin1String("Hurkx")) {
            srh.insert(QStringLiteral("tat"), true);
            srh.insert(QStringLiteral("mtn"), num(param(QStringLiteral("srh field n"), QStringLiteral("mt"), 0.25)));
            srh.insert(QStringLiteral("mtp"), num(param(QStringLiteral("srh field p"), QStringLiteral("mt"), 0.25)));
        } else if (on(QStringLiteral("srh field n")) && !active(QStringLiteral("srh field n")).isEmpty())
            out.notes << tr("%1: the Lumerical %2 trap-assisted tunnelling not here: none").arg(name, active(QStringLiteral("srh field n")));
        out.features.append(srh);
    }
    const double Cn = constantOrUniversal(QStringLiteral("auger n")), Cp = constantOrUniversal(QStringLiteral("auger p"));
    if (on(QStringLiteral("auger n")) && std::isfinite(Cn) && std::isfinite(Cp) && (Cn > 0 || Cp > 0))
        out.features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("auger")},
                                        {QStringLiteral("label"), tr("%1: Auger").arg(name)},
                                        {QStringLiteral("Cn"), QStringLiteral("%1[cm^6/s]").arg(num(Cn))},
                                        {QStringLiteral("Cp"), QStringLiteral("%1[cm^6/s]").arg(num(Cp))}});
    const double B = constantOrUniversal(QStringLiteral("radiative n"));
    if (on(QStringLiteral("radiative n")) && B > 0)
        out.features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("radiative")},
                                        {QStringLiteral("label"), tr("%1: radiative").arg(name)},
                                        {QStringLiteral("B"), QStringLiteral("%1[cm^3/s]").arg(num(B))}});
    // Impact ionization: Selberherr's α∞ e^(-(Ecrit/E)^β), one range.
    if (on(QStringLiteral("impact n")) && active(QStringLiteral("impact n")) == QLatin1String("Selberherr")) {
        QJsonObject ii{{QStringLiteral("type"), QStringLiteral("impact")}, {QStringLiteral("label"), tr("%1: impact ionization").arg(name)},
                       {QStringLiteral("model"), QStringLiteral("user")}, {QStringLiteral("E0"), QStringLiteral("1e30[V/m]")}};
        for (const char* c : {"n", "p"}) {
            const QString key = QStringLiteral("impact %1").arg(QLatin1String(c)), C = QLatin1String(c);
            const QString a = QStringLiteral("%1[1/cm]").arg(num(warm(key, QStringLiteral("alphaInf(300)"), QStringLiteral("eta_alphaInf"))));
            const QString b = QStringLiteral("%1[V/cm]").arg(num(warm(key, QStringLiteral("Ecrit(300)"), QStringLiteral("eta_Ecrit"))));
            ii.insert(QStringLiteral("a") + C, a);
            ii.insert(QStringLiteral("b") + C, b);
            ii.insert(QStringLiteral("a") + C + QStringLiteral("2"), a);
            ii.insert(QStringLiteral("b") + C + QStringLiteral("2"), b);
            ii.insert(QStringLiteral("beta") + C, num(param(key, QStringLiteral("beta"), 1)));
        }
        // (Its driving field: the Lumerical impact ionization's grad phi
        // (its Driving Field 1, inferred) as each carrier's quasi-Fermi
        // gradient; its E dot J (0: the APD's, its bandwidth script's) as
        // the field |E| - the APD so: its dark current 0.95 to 1.16 of the
        // Lumerical one's from -4.5 to -9.5 V, its breakdown at -9.9 V
        // (the Lumerical run's past -9.98 V); along each current, E·J/|J|,
        // 0.35 to 0.7 of it and -10.5 V.)
        const Value* dfn = model(QStringLiteral("impact n"));
        const Value* df = dfn ? dfn->field(QStringLiteral("driving")) : nullptr;
        const int code = df ? whole(df->toNumber(), -1000000000, 1000000000, -1) : 0;
        const bool gradPhi = code == 1;
        ii.insert(QStringLiteral("driving"), gradPhi ? QStringLiteral("fermi") : QStringLiteral("field"));
        out.features.append(ii);
        out.notes << (gradPhi ? tr("%1: impact ionization driven by each carrier's quasi-Fermi gradient (the Lumerical grad phi)").arg(name)
                              : tr("%1: impact ionization driven by the field |E| (the Lumerical driving field %2): the Lumerical APD's "
                                   "dark current and breakdown are |E|'s here; along each carrier's current, E·J/|J|, is Impact "
                                   "Ionization's Driving field too")
                                    .arg(name, !df ? tr("not given") : code == 0 ? tr("0, E dot J") : tr("%1, not one Qucs-S knows").arg(num(df->toNumber()))));
    }
    if (on(QStringLiteral("btbt")) && active(QStringLiteral("btbt")) == QLatin1String("Hurkx"))
        out.features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("btbt")},
                                        {QStringLiteral("label"), tr("%1: band-to-band tunnelling").arg(name)},
                                        {QStringLiteral("A"), num(param(QStringLiteral("btbt"), QStringLiteral("A")))},
                                        {QStringLiteral("B"), QStringLiteral("%1[V/cm]").arg(num(param(QStringLiteral("btbt"), QStringLiteral("B"))))},
                                        {QStringLiteral("gamma"), num(param(QStringLiteral("btbt"), QStringLiteral("eta"), 2.5))}});
    out.ok = true;
    return out;
}

LumericalImport buildCharge(const Scene& scene, const Object& solver)
{
    LumericalImport out;
    QStringList notes;
    // The region: the simulation region's (2D Y normal, the x-z plane).
    const Object* region = nullptr;
    const QString regionName = solver.text(QStringLiteral("simulation region"));
    for (const Object& o : scene.objects)
        if (o.kind == QLatin1String("region") && (regionName.isEmpty() || o.name() == regionName)) region = &o;
    Plane p;
    p.normal = 1;
    p.u = 0;
    p.v = 2;
    if (region) {
        // (A text, or a script's number: 1 2D x-normal, 2 y-normal, 3 z-normal, 4 3D.)
        QString dim = region->text(QStringLiteral("dimension")).toLower();
        if (const Value* d = region->prop(QStringLiteral("dimension")); d && d->isNumber())
            dim = QStringList{QStringLiteral("x-normal"), QStringLiteral("y-normal"), QStringLiteral("z-normal"), QStringLiteral("3d")}.value(whole(d->toNumber(2), 1, 4, 2) - 1, QStringLiteral("y-normal"));
        if (dim.contains(QLatin1String("x-normal")) || dim.contains(QLatin1String("x normal"))) {
            p.normal = 0;
            p.u = 1;
            p.v = 2;
        } else if (dim.contains(QLatin1String("z-normal")) || dim.contains(QLatin1String("z normal"))) {
            p.normal = 2;
            p.u = 0;
            p.v = 1;
        } else if (dim.contains(QLatin1String("3d"))) {
            out.error = tr("its simulation region is 3D: a 2D one's cross-section is read");
            return out;
        }
        const QLatin1String axes[3] = {QLatin1String("x"), QLatin1String("y"), QLatin1String("z")};
        std::pair<double, double> b[3];
        for (int a = 0; a < 3; ++a) b[a] = span(*region, axes[a], a == p.normal ? 0 : INFINITY);
        p.at = std::isfinite(b[p.normal].first + b[p.normal].second) ? (b[p.normal].first + b[p.normal].second) / 2 : 0;
        p.u0 = b[p.u].first;
        p.u1 = b[p.u].second;
        p.v0 = b[p.v].first;
        p.v1 = b[p.v].second;
    } else
        notes << tr("no simulation region: every structure in the x-z plane at y = 0");
    // The model materials: Lumerical's electrical ones by name - a
    // semiconductor with its models in the project, those; else the
    // library's of its name.
    const double T = solver.number(QStringLiteral("simulation temperature"), solver.number(QStringLiteral("temperature"), 300));
    const bool fermi = solver.number(QStringLiteral("fermi statistics"), 0) != 0;
    struct ModelMaterial {
        QJsonObject json;
        bool metal = false, insulator = false;
        double workfunction = NAN;
        QStringList notes;   // (said if it is used)
        std::optional<SemiconductorModels> models;
        QString alias;       // its material object's name (a surface recombination's)
    };
    std::map<QString, ModelMaterial> mm;
    for (const Object& o : scene.objects) {
        if (o.kind != QLatin1String("modelmaterial")) continue;
        ModelMaterial m;
        const QString name = o.name();
        m.alias = o.text(QStringLiteral("@alias"));
        if (o.text(QStringLiteral("@kind")) == QLatin1String("Semiconductor")) {
            const SemiconductorModels sm = semiconductorModels(o, T, fermi);
            if (sm.ok) {
                m.models = sm;
                m.json = sm.material;
                m.notes << sm.notes;
                m.workfunction = o.number(QStringLiteral("work function"), NAN);
                mm[name] = m;
                continue;
            }
        }
        const QString ct = o.text(QStringLiteral("@CT"));
        const QString alloy = o.text(QStringLiteral("@alloy"));
        double cx = 0, cy = 0;
        if (!alloy.isEmpty()) {
            const QString compound = compoundOf(alloy, o.number(QStringLiteral("@x"), 0), o.number(QStringLiteral("@y"), 0), &cx, &cy);
            if (!compound.isEmpty())
                m.json = QJsonObject{{QStringLiteral("label"), name}, {QStringLiteral("compound"), compound}, {QStringLiteral("x"), num(cx)}, {QStringLiteral("y"), num(cy)}};
            else
                m.notes << tr("%1: the alloy %2 not known here - set its material").arg(name, alloy);
        }
        if (m.json.isEmpty() && !ct.isEmpty()) {
            if (const QString ours = libraryOf(ct, true); !ours.isEmpty()) {
                m.json = QJsonObject{{QStringLiteral("label"), name}, {QStringLiteral("material"), ours}};
                m.notes << (ours != name ? tr("%1 as the library's %2").arg(name == ct ? ct : tr("%1 (%2)").arg(name, ct), ours) : QString());
            } else if (const QString compound = compoundOf(ct, o.number(QStringLiteral("mole frac constant"), 0), 0, &cx, &cy); !compound.isEmpty()) {
                m.json = QJsonObject{{QStringLiteral("label"), name}, {QStringLiteral("compound"), compound}, {QStringLiteral("x"), num(cx)}};
            } else if (o.text(QStringLiteral("@kind")) != QLatin1String("Conductor"))
                m.notes << tr("%1: the Lumerical material %2 not in the library - set its properties").arg(name, ct);
        }
        if (m.json.isEmpty()) m.json = QJsonObject{{QStringLiteral("label"), name}};
        const QString lib = m.json.value(QStringLiteral("material")).toString();
        static const QStringList metals = {QStringLiteral("Aluminium"), QStringLiteral("Copper"), QStringLiteral("Gold"), QStringLiteral("Silver")};
        static const QStringList insulators = {QStringLiteral("Silicon dioxide"), QStringLiteral("Fused silica"), QStringLiteral("Silicon nitride"),
                                               QStringLiteral("Alumina (96%)"), QStringLiteral("Air"), QStringLiteral("Lithium niobate")};
        // (Its kind: the project's own, else the library's.)
        const QString kind = o.text(QStringLiteral("@kind"));
        m.metal = kind.isEmpty() ? metals.contains(lib) || ct.contains(QLatin1String("(Aluminium)")) || ct.contains(QLatin1String("CRC")) : kind == QLatin1String("Conductor");
        m.insulator = kind.isEmpty() ? insulators.contains(lib) : kind == QLatin1String("Insulator");
        if (m.json.size() == 1 && kind == QLatin1String("Insulator") && o.prop(QStringLiteral("@epsilonr")))
            m.json.insert(QStringLiteral("epsilonr"), num(o.number(QStringLiteral("@epsilonr"))));
        m.workfunction = o.number(QStringLiteral("work function"), NAN);
        mm[name] = m;
    }
    // The structures (their mesh order the DEVICE's own).
    QSet<QString> used;
    auto order = [&](const Object& o) { return whole(o.number(QStringLiteral("mesh order"), 2), -1000000000, 1000000000, 2); };
    std::vector<Shape> shapes = structures(scene, p, order, used, notes);
    if (shapes.empty()) {
        out.error = tr("no structure in its simulation region");
        return out;
    }
    double u0 = INFINITY, v0 = INFINITY, u1 = -INFINITY, v1 = -INFINITY;
    for (const Shape& s : shapes) {
        u0 = std::min(u0, s.x0);
        v0 = std::min(v0, s.y0);
        u1 = std::max(u1, s.x1);
        v1 = std::max(v1, s.y1);
    }
    const double size = std::max(u1 - u0, v1 - v0);
    const std::pair<QString, double> unitScale = unitFor(size);
    const QString unit = unitScale.first;
    const double scale = unitScale.second;
    auto isMetal = [&](const Shape& s) { return mm.count(s.material) && mm.at(s.material).metal; };
    auto touches = [&](const Shape& a, const Shape& b) {
        const double e = 1e-9 * size;
        return a.x0 <= b.x1 + e && b.x0 <= a.x1 + e && a.y0 <= b.y1 + e && b.y0 <= a.y1 + e;
    };
    auto overlaps = [&](const Shape& a, const Shape& b) {
        const double e = 1e-9 * size;
        return std::min(a.x1, b.x1) - std::max(a.x0, b.x0) > e && std::min(a.y1, b.y1) - std::max(a.y0, b.y0) > e;
    };
    auto insulator = [&](const Shape& s) { return mm.count(s.material) && mm.at(s.material).insulator; };
    // The contacts: on their solids. A thin metal strip touching the device
    // (a contact's own, as an export makes it) is left out, the contact on
    // the device's boundaries within it; another solid's the boundaries
    // between it and the device.
    struct Contact {
        const Object* o;
        QString name, solid;
        QJsonObject between, box;   // (its box's: its strip left out)
        QString strip;
        double footprint = 0;       // (the strip's long side, the unit's)
        bool gate = false;
        double phim = NAN;
    };
    std::vector<Contact> contacts;
    QSet<QString> dropped;
    for (const Object& o : scene.objects) {
        if (o.kind != QLatin1String("contact")) continue;
        Contact c;
        c.o = &o;
        c.name = o.name();
        c.solid = o.text(QStringLiteral("solid"));
        const auto it = std::find_if(shapes.begin(), shapes.end(), [&](const Shape& s) { return s.name == c.solid; });
        if (it == shapes.end()) {
            notes << tr("contact %1: its solid %2 is not in the region: left out").arg(c.name, c.solid);
            continue;
        }
        const Shape& s = *it;
        // (Ohmic on the semiconductors it touches; else a gate, on the
        // insulators.)
        QStringList semis, insulators;
        bool alone = true;
        for (const Shape& t : shapes) {
            if (&t == &s) continue;
            if (overlaps(s, t)) alone = false;
            if (isMetal(t) || !touches(s, t)) continue;
            (insulator(t) ? insulators : semis) << t.label;
        }
        if (semis.isEmpty() && insulators.isEmpty()) {
            notes << tr("contact %1: its solid %2 touches nothing of the device: left out").arg(c.name, c.solid);
            continue;
        }
        c.gate = semis.isEmpty();
        const QStringList device = c.gate ? insulators : semis;
        if (c.gate) c.phim = mm.count(s.material) ? mm.at(s.material).workfunction : NAN;
        // (A thin metal strip on the device and in nothing - an export's -
        // left out: the contact on the boundaries it covers.)
        const bool thin = std::min(s.x1 - s.x0, s.y1 - s.y0) < 0.05 * size;
        c.between = QJsonObject{{QStringLiteral("between"), QJsonArray{QJsonArray{s.label}, QJsonArray::fromStringList(device)}}};
        if (thin && alone && isMetal(s)) {
            dropped.insert(s.label);
            const double e = 1e-6 * size;
            c.strip = s.label;
            c.footprint = std::max(s.x1 - s.x0, s.y1 - s.y0) / scale;
            c.box = QJsonObject{{QStringLiteral("box"), QJsonArray{num((s.x0 - e) / scale), num((s.y0 - e) / scale), num((s.x1 + e) / scale), num((s.y1 + e) / scale)}}};
        }
        contacts.push_back(c);
    }
    const QStringList pre = notes;
    auto assemble = [&pre, &mm, &shapes, &dropped, &scale, &isMetal, &insulator, &unit, &scene, &p, &size, &used, &contacts, &solver, &T, &u0, &u1, &v0, &v1]() -> LumericalImport {
    LumericalImport made;
    QStringList notes = pre;
    QJsonArray geometry, materials, features;
    QStringList device, metals, semiconductors, library;
    QSet<QString> said;
    QString last;
    QStringList lastOn;
    auto flush = [&]() {
        if (last.isEmpty()) return;
        QJsonObject m = mm.count(last) ? mm.at(last).json : QJsonObject{{QStringLiteral("label"), last}};
        if (!mm.count(last)) {
            if (const QString ours = libraryOf(last, true); !ours.isEmpty()) m = QJsonObject{{QStringLiteral("material"), ours}};
        }
        m.insert(QStringLiteral("selection"), only(lastOn));
        materials.append(m);
    };
    for (const Shape& s : shapes) {
        if (dropped.contains(s.label)) continue;
        geometry.append(shapeJson(s, scale));
        // (One material for each run of the same: the order kept.)
        if (s.material != last) {
            flush();
            last = s.material;
            lastOn.clear();
        }
        lastOn << s.label;
        if (!said.contains(s.material)) {
            said.insert(s.material);
            if (mm.count(s.material))
                for (const QString& n : mm.at(s.material).notes)
                    if (!n.isEmpty()) (n.contains(QLatin1String("as the library's")) ? library : notes) << n;
            if (!mm.count(s.material) && libraryOf(s.material, true).isEmpty()) notes << tr("%1: its material %2 is not a model material: set it").arg(s.name, s.material);
        }
        (isMetal(s) ? metals : device) << s.label;
        if (!isMetal(s) && !insulator(s)) semiconductors << s.label;
    }
    flush();
    if (!library.isEmpty()) notes << tr("Lumerical materials as the library's: %1").arg(library.join(QStringLiteral(", ")));
    if (!dropped.isEmpty()) notes << tr("the contacts' thin metal strips left out: each contact on the boundaries it covered");
    geometry.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("form union")}});
    // The doping.
    const QLatin1String axes[3] = {QLatin1String("x"), QLatin1String("y"), QLatin1String("z")};
    auto inUnit = [&](double m) { return num(m / scale); };
    // (In an expression a plain number is SI: a length there carries its unit.)
    auto length = [&](double m) { return QStringLiteral("%1[%2]").arg(num(m / scale), unit); };
    QJsonArray functions;   // (tables: a sampled doping's, an imported generation's)
    int sampled = 0;
    /// A dataset's slice at the plane (its values f on its axes x, y, z, x
    /// fastest; relative to the object's middle where it says so), as a
    /// table of the plane's two lengths (the model's unit) - its bounds in
    /// \a lo and \a hi (m); empty where its values and axes disagree.
    auto slice = [&p, &scale, &axes](const Object& o, const Value* f, const Value* const X[3], double factor, double lo[2], double hi[2]) {
        QJsonArray grid;
        const std::size_t n[3] = {X[0]->m.size(), X[1]->m.size(), X[2]->m.size()};
        if (f->m.size() != n[0] * n[1] * n[2] || !n[0] || !n[1] || !n[2]) return grid;
        double offset[3] = {0, 0, 0};
        if (o.number(QStringLiteral("use relative coordinates"), 0) != 0)
            for (int a = 0; a < 3; ++a) offset[a] = o.number(axes[a], 0);
        // (The slice nearest the plane along its normal: its data constant along it, one.)
        const std::vector<double>& along = X[p.normal]->m;
        std::size_t k = 0;
        for (std::size_t i = 1; i < along.size(); ++i)
            if (std::abs(along[i] + offset[p.normal] - p.at) < std::abs(along[k] + offset[p.normal] - p.at)) k = i;
        lo[0] = lo[1] = INFINITY;
        hi[0] = hi[1] = -INFINITY;
        for (std::size_t i = 0; i < n[p.u]; ++i)
            for (std::size_t j = 0; j < n[p.v]; ++j) {
                std::size_t idx[3];
                idx[p.u] = i;
                idx[p.v] = j;
                idx[p.normal] = k;
                const double g = f->m[idx[0] + n[0] * (idx[1] + n[1] * idx[2])] * factor;
                const double u = X[p.u]->m[i] + offset[p.u], v = X[p.v]->m[j] + offset[p.v];
                lo[0] = std::min(lo[0], u);
                hi[0] = std::max(hi[0], u);
                lo[1] = std::min(lo[1], v);
                hi[1] = std::max(hi[1], v);
                grid.append(QJsonArray{u / scale, v / scale, std::isfinite(g) ? g : 0.0});
            }
        return grid;
    };
    for (const Object& o : scene.objects) {
        const bool dope = o.kind == QLatin1String("dope"), diffusion = o.kind == QLatin1String("diffusion");
        if (!dope && !diffusion && o.kind != QLatin1String("importdope")) continue;
        const QString name = o.name();
        const QString t = o.text(QStringLiteral("dopant type")).toLower();
        const QString dopant = t.startsWith(QLatin1Char('n')) || t == QLatin1String("0") || t.contains(QLatin1String("donor")) ? QStringLiteral("donor") : QStringLiteral("acceptor");
        if (o.kind == QLatin1String("importdope")) {
            // A sampled profile - a script's importdataset of a rectilinear
            // dataset, its attribute N (1/m³, as the script's values are):
            // the slice at the plane, a table of x and y, nought outside its
            // data. (A project's own, its data not read: said.)
            const Value* data = o.prop(QStringLiteral("@dataset"));
            const Value* N = data ? data->field(QStringLiteral("N")) : nullptr;
            const Value* X[3] = {data ? data->field(QStringLiteral("x")) : nullptr, data ? data->field(QStringLiteral("y")) : nullptr,
                                 data ? data->field(QStringLiteral("z")) : nullptr};
            if (!N || !X[0] || !X[1] || !X[2]) {
                notes << tr("%1: an imported doping profile without its data (a dataset of N on x, y, z) is not read - give it as an analytic doping").arg(name);
                continue;
            }
            double lo[2], hi[2];
            const QJsonArray grid = slice(o, N, X, 1, lo, hi);
            if (grid.isEmpty()) {
                notes << tr("%1: its doping's data and coordinates do not agree: left out").arg(name);
                continue;
            }
            const double e = 1e-9 * size;
            const QRectF box(QPointF(lo[0] - e, lo[1] - e), QPointF(hi[0] + e, hi[1] + e));
            QStringList on;
            for (const Shape& s : shapes)
                if (!dropped.contains(s.label) && !isMetal(s) && !insulator(s) && box.intersects(QRectF(QPointF(s.x0, s.y0), QPointF(s.x1, s.y1)))) on << s.label;
            if (on.isEmpty()) {
                notes << tr("%1: on no domain of the device: left out").arg(name);
                continue;
            }
            const QString fn = label(QStringLiteral("N_") + contactName(name), used);
            functions.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("interpolation")},
                                         {QStringLiteral("name"), fn},
                                         {QStringLiteral("dimension"), QStringLiteral("2")},
                                         {QStringLiteral("grid"), grid},
                                         {QStringLiteral("argunit"), QStringLiteral("%1, %1").arg(unit)},
                                         {QStringLiteral("unit"), QStringLiteral("1/m^3")}});
            features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("doping")},
                                        {QStringLiteral("label"), name},
                                        {QStringLiteral("dopant"), dopant},
                                        {QStringLiteral("profile"), QStringLiteral("expression")},
                                        {QStringLiteral("N"), QStringLiteral("%1(x, y)*(x >= %2 && x <= %3 && y >= %4 && y <= %5)")
                                                                  .arg(fn, length(lo[0] - e), length(hi[0] + e), length(lo[1] - e), length(hi[1] + e))},
                                        {QStringLiteral("selection"), only(on)}});
            ++sampled;
            continue;
        }
        std::pair<double, double> b[3];
        for (int a = 0; a < 3; ++a) b[a] = span(o, axes[a], INFINITY);
        const double N0 = o.number(QStringLiteral("concentration"), NAN);
        if (!std::isfinite(N0)) {
            notes << tr("%1: no concentration: left out").arg(name);
            continue;
        }
        // (Restricted to a solid: its domains.)
        QStringList on;
        const QString solid = o.text(QStringLiteral("volume solid"));
        const bool restricted = !solid.isEmpty() && o.text(QStringLiteral("volume type")).toLower().contains(QLatin1String("solid"));
        QRectF box(QPointF(b[p.u].first, b[p.v].first), QPointF(b[p.u].second, b[p.v].second));
        for (const Shape& s : shapes) {
            if (dropped.contains(s.label) || isMetal(s) || insulator(s)) continue;
            if (restricted && s.name != solid) continue;
            const QRectF r(QPointF(s.x0, s.y0), QPointF(s.x1, s.y1));
            if (box.intersects(r) || (std::isinf(box.width()) || std::isinf(box.height()))) on << s.label;
        }
        if (on.isEmpty()) {
            notes << tr("%1: on no domain of the device: left out").arg(name);
            continue;
        }
        QJsonObject f{{QStringLiteral("type"), QStringLiteral("doping")}, {QStringLiteral("label"), name}, {QStringLiteral("dopant"), dopant}, {QStringLiteral("selection"), only(on)}};
        if (dope) {
            // The box over its domains: uniform; within them, a box's expression.
            bool covers = true;
            for (const Shape& s : shapes)
                if (on.contains(s.label))
                    covers = covers && box.left() <= s.x0 + 1e-9 * size && box.right() >= s.x1 - 1e-9 * size && box.top() <= s.y0 + 1e-9 * size
                             && box.bottom() >= s.y1 - 1e-9 * size;
            if (covers) {
                f.insert(QStringLiteral("profile"), QStringLiteral("uniform"));
                f.insert(QStringLiteral("N0"), QStringLiteral("%1[1/m^3]").arg(num(N0)));
            } else {
                f.insert(QStringLiteral("profile"), QStringLiteral("expression"));
                f.insert(QStringLiteral("N"), QStringLiteral("%1[1/m^3]*(x >= %2 && x <= %3 && y >= %4 && y <= %5)")
                                                  .arg(num(N0), length(box.left()), length(box.right()), length(box.top()), length(box.bottom())));
            }
        } else {
            // A diffusion: its plateau the box less the junction width on
            // each side but its source face's (out of the plane: every side).
            const double dj = o.number(QStringLiteral("junction width"), NAN), Nb = o.number(QStringLiteral("ref concentration"), NAN);
            if (!(dj > 0) || !(Nb > 0) || !(N0 > Nb)) {
                notes << tr("%1: its junction width and reference concentration: left out").arg(name);
                continue;
            }
            int face = whole(o.number(QStringLiteral("source face"), 6), 1, 6, 6);   // 1 x min ... 6 z max
            const int faceAxis = (face - 1) / 2, faceHigh = (face - 1) % 2;
            double x0 = box.left() + dj, x1 = box.right() - dj, y0 = box.top() + dj, y1 = box.bottom() - dj;
            if (faceAxis == p.u) (faceHigh ? x1 : x0) += faceHigh ? dj : -dj;
            if (faceAxis == p.v) (faceHigh ? y1 : y0) += faceHigh ? dj : -dj;
            // (No plateau - a profile from its source face - is a box of no
            // depth: a box as deep as its junction width kept so.)
            const double slack = 1e-9 * std::max(dj, size);
            if (x1 < x0 && x1 > x0 - slack) x1 = x0;
            if (y1 < y0 && y1 > y0 - slack) y1 = y0;
            if (!(x1 >= x0) || !(y1 >= y0)) {
                notes << tr("%1: its box narrower than its junction width: left out").arg(name);
                continue;
            }
            if (o.text(QStringLiteral("distribution function")).toLower().contains(QLatin1String("erf")))
                notes << tr("%1: the Lumerical erfc diffusion as a Gaussian").arg(name);
            f.insert(QStringLiteral("profile"), QStringLiteral("box"));
            f.insert(QStringLiteral("N0"), QStringLiteral("%1[1/m^3]").arg(num(N0)));
            f.insert(QStringLiteral("Nb"), QStringLiteral("%1[1/m^3]").arg(num(Nb)));
            f.insert(QStringLiteral("dj"), QStringLiteral("%1[um]").arg(num(dj * 1e6)));
            f.insert(QStringLiteral("position"), QJsonArray{inUnit(x0), inUnit(y0)});
            f.insert(QStringLiteral("size"), QJsonArray{inUnit(x1 - x0), inUnit(y1 - y0)});
        }
        features.append(f);
    }
    // Surface recombination: on the boundaries between its two materials
    // (by their material objects' names) - between a semiconductor and an
    // insulator or another; at a metal, its contact's carriers at
    // equilibrium already.
    std::map<QString, QStringList> labelsOf;   // by material and by its object's name
    for (const Shape& s : shapes) {
        if (dropped.contains(s.label)) continue;
        labelsOf[s.material] << s.label;
        if (mm.count(s.material) && !mm.at(s.material).alias.isEmpty() && mm.at(s.material).alias != s.material) labelsOf[mm.at(s.material).alias] << s.label;
    }
    int recombination = 0, warmed = 0, atContacts = 0;
    QStringList atMetals;
    for (const Object& o : scene.objects) {
        if (o.kind != QLatin1String("surfacerec")) continue;
        const QString a = o.text(QStringLiteral("material 1")), b = o.text(QStringLiteral("material 2"));
        if (a.isEmpty() || b.isEmpty()) {
            notes << tr("%1: its two materials not given (surface type %2): left out").arg(o.name()).arg(o.number(QStringLiteral("surface type"), -1));
            continue;
        }
        auto metalNamed = [&](const QString& n) {
            for (const auto& [name, m] : mm)
                if ((name == n || m.alias == n) && m.metal) return true;
            return false;
        };
        // (Its "apply to majority carriers": at a metal off - the contact's
        // minority carriers alone, its majority held -, elsewhere on.)
        const bool atMetal = metalNamed(a) || metalNamed(b);
        const double majority = o.number(QStringLiteral("apply to majority carriers"), NAN);
        if (atMetal && (!labelsOf.count(a) || !labelsOf.count(b))) {
            atMetals << o.name();
            continue;
        }
        if (!labelsOf.count(a) || !labelsOf.count(b)) {
            notes << tr("%1: between %2 and %3, not both in the region: left out").arg(o.name(), a, b);
            continue;
        }
        if (atMetal && majority == 1)
            notes << tr("%1: at a metal its majority carriers too in Lumerical - here the contact holds them, its minority carriers "
                        "alone recombine")
                         .arg(o.name());
        if (!atMetal && majority == 0)
            notes << tr("%1: its minority carriers alone in Lumerical - here both, as Lumerical advises at an insulator's or a "
                        "semiconductor's interface")
                         .arg(o.name());
        const double Sn = o.number(QStringLiteral("electron velocity"), NAN), Sp = o.number(QStringLiteral("hole velocity"), NAN);
        if (!(Sn >= 0) || !(Sp >= 0)) continue;
        // (Its temperature model: S (T/300 K)^η, each carrier's η.)
        const bool warm = o.number(QStringLiteral("enable temperature model"), 0) != 0;
        const double etaN = warm ? o.number(QStringLiteral("electron eta"), 0) : 0, etaP = warm ? o.number(QStringLiteral("hole eta"), 0) : 0;
        if (warm && (etaN != 0 || etaP != 0)) ++warmed;
        features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("surfacerec")},
                                    {QStringLiteral("label"), o.name()},
                                    {QStringLiteral("Sn"), ofT(Sn, QStringLiteral("m/s"), etaN)},
                                    {QStringLiteral("Sp"), ofT(Sp, QStringLiteral("m/s"), etaP)},
                                    {QStringLiteral("Et"), QStringLiteral("%1[eV]").arg(num(o.number(QStringLiteral("trap offset"), 0)))},
                                    {QStringLiteral("majority"), !atMetal},
                                    {QStringLiteral("selection"), QJsonObject{{QStringLiteral("between"), QJsonArray{QJsonArray::fromStringList(labelsOf.at(a)), QJsonArray::fromStringList(labelsOf.at(b))}}}}});
        ++recombination;
        if (atMetal) ++atContacts;
    }
    if (recombination)
        notes << tr("%1 surface recombination objects as Surface Recombination on the boundaries between their materials (each "
                    "carrier's velocity, the SRH form)")
                     .arg(recombination);
    if (warmed) notes << tr("%1 of them with their temperature model: S (T/300 K)^η").arg(warmed);
    if (atContacts)
        notes << tr("%1 of them at a metal: its contact's minority carriers by it, its majority held at equilibrium (Apply to majority "
                    "carriers off)")
                     .arg(atContacts);
    if (!atMetals.isEmpty())
        notes << tr("surface recombination at the metal contacts left out (%1): their metal not in the region, their carriers at "
                    "equilibrium")
                     .arg(atMetals.join(QStringLiteral(", ")));
    // Optical generation imported: the data's slice in the plane (its
    // coordinates its own), a table of two arguments, times its scale
    // factor (the light's power, W), nought outside its data.
    if (sampled) notes << tr("%1 imported doping profiles (sampled) as tables of x and y, nought outside their data").arg(sampled);
    for (const Object& o : scene.objects) {
        if (o.kind != QLatin1String("generation")) continue;
        // (A project's: its data the object's own; a script's: its
        // importdataset's, G on x, y, z.)
        const Value* data = o.prop(QStringLiteral("@dataset"));
        const Value* G = data ? data->field(QStringLiteral("G")) : o.prop(QStringLiteral("@G"));
        const Value* X[3] = {data ? data->field(QStringLiteral("x")) : o.prop(QStringLiteral("@x")), data ? data->field(QStringLiteral("y")) : o.prop(QStringLiteral("@y")),
                             data ? data->field(QStringLiteral("z")) : o.prop(QStringLiteral("@z"))};
        if (!G || !X[0] || !X[1] || !X[2]) continue;
        const std::size_t n[3] = {X[0]->m.size(), X[1]->m.size(), X[2]->m.size()};
        const double power = o.number(QStringLiteral("scale factor"), 1) * o.number(QStringLiteral("source power scaling"), 1);
        double lo[2], hi[2];
        const QJsonArray grid = slice(o, G, X, power, lo, hi);
        if (grid.isEmpty()) {
            notes << tr("%1: its generation's data and coordinates do not agree: left out").arg(o.name());
            continue;
        }
        const QString fn = label(QStringLiteral("G_") + contactName(o.name()), used);
        functions.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("interpolation")},
                                     {QStringLiteral("name"), fn},
                                     {QStringLiteral("dimension"), QStringLiteral("2")},
                                     {QStringLiteral("grid"), grid},
                                     {QStringLiteral("argunit"), QStringLiteral("%1, %1").arg(unit)},
                                     {QStringLiteral("unit"), QStringLiteral("1/(m^3*s)")}});
        const double e = 1e-9 * size;
        QStringList on;
        const QRectF box(QPointF(lo[0] - e, lo[1] - e), QPointF(hi[0] + e, hi[1] + e));
        for (const Shape& s : shapes)
            if (!dropped.contains(s.label) && !isMetal(s) && !insulator(s) && box.intersects(QRectF(QPointF(s.x0, s.y0), QPointF(s.x1, s.y1)))) on << s.label;
        if (on.isEmpty()) {
            notes << tr("%1: its generation meets no semiconductor: left out").arg(o.name());
            continue;
        }
        const bool off = o.number(QStringLiteral("enabled"), 1) == 0;
        features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("generation")},
                                    {QStringLiteral("enabled"), !off},
                                    {QStringLiteral("label"), o.name()},
                                    {QStringLiteral("G"), QStringLiteral("%1(x, y)*(x >= %2 && x <= %3 && y >= %4 && y <= %5)")
                                                              .arg(fn, length(lo[0] - e), length(hi[0] + e), length(lo[1] - e), length(hi[1] + e))},
                                    {QStringLiteral("selection"), only(on)}});
        notes << tr("%1: its imported optical generation (%2 x %3 x %4 points) the slice at the plane, a function %5 of x and y, "
                    "times its scale factor %6 W")
                     .arg(o.name())
                     .arg(n[0])
                     .arg(n[1])
                     .arg(n[2])
                     .arg(fn, num(power))
                 + (off ? tr(" - off, as the project was saved: switch it on for the light") : QString());
    }
    // The contacts: their voltages - a sweep's parameter each.
    QJsonArray parameters, sweeps;
    QString swept;
    QSet<QString> usedParameters;
    for (const Contact& c : contacts) {
        const Object& o = *c.o;
        const QString sweep = o.text(QStringLiteral("sweep type")).toLower();
        QString V0 = QStringLiteral("%1[V]").arg(num(o.number(QStringLiteral("voltage"), 0)));
        QStringList values;
        if (sweep == QLatin1String("value") || sweep == QLatin1String("2")) {
            if (const Value* t = o.prop(QStringLiteral("value table")))
                for (double v : t->m) values << num(v);
        } else if (sweep == QLatin1String("range") || sweep == QLatin1String("1")) {
            const double a = o.number(QStringLiteral("range start"), 0), b2 = o.number(QStringLiteral("range stop"), 0);
            // (A sweep's points no more than a sweep takes, 10,000: two
            // billion were taken, the import building them for minutes - the
            // hunt of 19 October, F2.)
            int n = whole(o.number(QStringLiteral("range num points"), 1), 1, 10000, -1);
            if (n < 0) {
                notes << tr("contact %1: its range of %2 points more than the 10,000 a sweep takes - its range start alone").arg(c.name, num(o.number(QStringLiteral("range num points"), 1)));
                n = 1;
            }
            for (int i = 0; i < n; ++i) values << num(n == 1 ? a : a + (b2 - a) * i / (n - 1));
        }
        if (values.size() > 1) {
            const QString par = label(QStringLiteral("V_") + contactName(c.name), usedParameters);
            parameters.append(QJsonObject{{QStringLiteral("name"), par}, {QStringLiteral("expression"), values.first() + QStringLiteral("[V]")}, {QStringLiteral("description"), tr("%1's voltage").arg(c.name)}});
            sweeps.append(QJsonArray{par, values.join(QStringLiteral(", ")), QStringLiteral("V")});
            V0 = par;
            if (swept.isEmpty()) swept = c.name;
        }
        QJsonObject f{{QStringLiteral("type"), c.gate ? QStringLiteral("gate") : QStringLiteral("ohmic")},
                      {QStringLiteral("label"), c.name},
                      {QStringLiteral("name"), contactName(c.name)},
                      {QStringLiteral("V0"), V0},
                      {QStringLiteral("selection"), dropped.contains(c.strip) ? c.box : c.between}};
        if (c.gate) f.insert(QStringLiteral("phim"), QStringLiteral("%1[eV]").arg(num(std::isfinite(c.phim) ? c.phim : 4.05)));
        if (c.gate && !std::isfinite(c.phim)) notes << tr("contact %1: a gate - its metal's work function not given: 4.05 eV").arg(c.name);
        features.append(f);
    }
    if (sweeps.size() > 1) notes << tr("its contacts sweep together in Lumerical (each its own table): here every combination");
    // Each semiconductor's Lumerical models on its domains; the library's
    // (an SRH of its lifetimes) on those whose project has none.
    {
        std::map<QString, QStringList> domainsOf;
        for (const Shape& s : shapes)
            if (!dropped.contains(s.label) && !isMetal(s) && !insulator(s)) domainsOf[s.material] << s.label;
        QStringList library;
        for (const auto& [material, labels] : domainsOf) {
            const auto it = mm.find(material);
            if (it == mm.end() || !it->second.models) {
                library << labels;
                continue;
            }
            QJsonObject mf = it->second.models->model;
            mf.insert(QStringLiteral("selection"), only(labels));
            features.append(mf);
            for (const QJsonValue& v : it->second.models->features) {
                QJsonObject f = v.toObject();
                f.insert(QStringLiteral("selection"), only(labels));
                features.append(f);
            }
        }
        // (A Qucs-S export's own models carried: theirs, not the library's.)
        if (!library.isEmpty() && scene.carried.isEmpty()) {
            features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("srh")}, {QStringLiteral("label"), tr("Recombination")}, {QStringLiteral("selection"), only(library)}});
            notes << tr("%1: the library's models (mobility, recombination), the project having none of its own - check them").arg(library.join(QStringLiteral(", ")));
        }
    }
    // A Qucs-S export's own line: the model's semiconductor features,
    // settings, parameters, functions and mesh, their selections in the
    // structures' names - here the shapes' -, a box's corners in metres.
    QJsonObject carriedMesh, carriedSettings;
    if (!scene.carried.isEmpty()) {
        QJsonObject c = scene.carried;
        // (Its semiconductor's own tag, here "semi": its expressions naming it so.)
        const QString was = c.value(QStringLiteral("tag")).toString();
        if (!was.isEmpty() && was != QLatin1String("semi")) {
            QString text = QString::fromUtf8(QJsonDocument(c).toJson(QJsonDocument::Compact));
            text.replace(QRegularExpression(QStringLiteral("\\b%1\\.").arg(QRegularExpression::escape(was))), QStringLiteral("semi."));
            c = QJsonDocument::fromJson(text.toUtf8()).object();
            notes << tr("its semiconductor tagged %1 in the model: here semi, its own expressions so").arg(was);
        }
        std::map<QString, QString> labelOf;
        for (const Shape& s : shapes) labelOf[s.name] = s.label;
        const double perMetre = 1 / scale;
        std::function<QJsonObject(const QJsonObject&)> remap = [&labelOf, &remap, perMetre](const QJsonObject& rule) {
            auto names = [&labelOf](const QJsonValue& v) {
                QJsonArray a;
                for (const QJsonValue& n : v.toArray())
                    if (const auto it = labelOf.find(n.toString()); it != labelOf.end()) a.append(it->second);
                return a;
            };
            QJsonObject out;
            for (auto it = rule.begin(); it != rule.end(); ++it) {
                const QString key = it.key();
                if (key == QLatin1String("objects") || key == QLatin1String("only") || key == QLatin1String("of")) out.insert(key, names(it.value()));
                else if (key == QLatin1String("between")) {
                    const QJsonArray pair = it.value().toArray();
                    out.insert(key, QJsonArray{names(pair.at(0)), names(pair.at(1))});
                } else if (key == QLatin1String("adjacent") || key == QLatin1String("except")) out.insert(key, remap(it.value().toObject()));
                else if (key == QLatin1String("box")) {
                    QJsonArray b;
                    for (const QJsonValue& v : it.value().toArray()) b.append(num(v.toDouble() * perMetre));
                    out.insert(key, b);
                } else out.insert(key, it.value());
            }
            return out;
        };
        // (The script's objects a carried one stands for - a sampled doping,
        // a generation, a surface recombination on every interface of two
        // materials - out, their tables with them: the exact one in their
        // place.)
        QSet<QString> replaced, unused;
        for (const QJsonValue& v : c.value(QStringLiteral("features")).toArray())
            if (const QString n = v.toObject().value(QStringLiteral("native")).toString(); !n.isEmpty()) replaced.insert(n);
        if (!replaced.isEmpty()) {
            static const QRegularExpression table(QStringLiteral("^([A-Za-z_][A-Za-z0-9_]*)\\("));
            QJsonArray kept;
            QStringList gone;
            int sampledGone = 0, surfaceGone = 0;
            for (const QJsonValue& v : features) {
                const QJsonObject f = v.toObject();
                const QString l = f.value(QStringLiteral("label")).toString(), type = f.value(QStringLiteral("type")).toString();
                const bool stands = (type == QLatin1String("doping") || type == QLatin1String("surfacerec") || type == QLatin1String("generation"))
                                    && std::any_of(replaced.begin(), replaced.end(), [&l](const QString& r) { return l == r || l.startsWith(r + QLatin1Char(' ')); });
                if (!stands) {
                    kept.append(f);
                    continue;
                }
                gone << l;
                surfaceGone += type == QLatin1String("surfacerec");
                for (const char* key : {"N", "G"})
                    if (const QRegularExpressionMatch m = table.match(f.value(QLatin1String(key)).toString()); m.hasMatch()) {
                        unused.insert(m.captured(1));
                        sampledGone += key[0] == 'N';
                    }
            }
            features = kept;
            // (Their notes theirs no longer: the model's own in their place, said.)
            QStringList said;
            for (const QString& note : notes) {
                const bool theirs = std::any_of(gone.begin(), gone.end(), [&note](const QString& l) { return note.startsWith(l + QLatin1Char(':')); })
                                    || (sampledGone == sampled && note.contains(QLatin1String("imported doping profiles (sampled)")))
                                    || (surfaceGone == recombination && (note.contains(QLatin1String("surface recombination objects as Surface Recombination"))
                                                                         || note.contains(QLatin1String("of them with their temperature model"))));
                if (!theirs) said << note;
            }
            notes = said;
            if (!gone.isEmpty())
                notes << tr("%1 of the script's objects as the model has them (its Qucs-S line), not as sampled or on every interface of two materials: %2")
                             .arg(gone.size())
                             .arg(gone.join(QStringLiteral(", ")));
            QJsonArray fs;
            for (const QJsonValue& v : functions)
                if (!unused.contains(v.toObject().value(QStringLiteral("name")).toString())) fs.append(v);
            functions = fs;
        }
        int n = 0;
        for (const QJsonValue& v : c.value(QStringLiteral("features")).toArray()) {
            QJsonObject f = v.toObject();
            f.remove(QStringLiteral("native"));
            if (f.contains(QStringLiteral("selection"))) f.insert(QStringLiteral("selection"), remap(f.value(QStringLiteral("selection")).toObject()));
            ++n;
            // (A default one - the semiconductor's own model, its insulation - in
            // its place; the others a tag of their own here.)
            if (f.value(QStringLiteral("default")).toBool()) {
                bool placed = false;
                for (int i = 0; i < features.size() && !placed; ++i)
                    if (features[i].toObject().value(QStringLiteral("default")).toBool() && features[i].toObject().value(QStringLiteral("type")) == f.value(QStringLiteral("type"))) {
                        features[i] = f;
                        placed = true;
                    }
                if (!placed) features.append(f);
                continue;
            }
            f.remove(QStringLiteral("tag"));
            features.append(f);
        }
        carriedSettings = c.value(QStringLiteral("settings")).toObject();
        QSet<QString> named, fnames;
        for (const QJsonValue& v : parameters) named.insert(v.toObject().value(QStringLiteral("name")).toString());
        for (const QJsonValue& v : c.value(QStringLiteral("parameters")).toArray())
            if (!named.contains(v.toObject().value(QStringLiteral("name")).toString())) parameters.append(v);
        for (const QJsonValue& v : functions) fnames.insert(v.toObject().value(QStringLiteral("name")).toString());
        for (const QJsonValue& v : c.value(QStringLiteral("functions")).toArray())
            if (!fnames.contains(v.toObject().value(QStringLiteral("name")).toString())) functions.append(v);
        if (c.value(QStringLiteral("unit")).toString() == unit) {
            carriedMesh = c.value(QStringLiteral("mesh")).toObject();
            QJsonArray mf;
            for (const QJsonValue& v : carriedMesh.value(QStringLiteral("features")).toArray()) {
                QJsonObject f = v.toObject();
                if (f.contains(QStringLiteral("selection"))) f.insert(QStringLiteral("selection"), remap(f.value(QStringLiteral("selection")).toObject()));
                f.remove(QStringLiteral("tag"));
                mf.append(f);
            }
            carriedMesh.insert(QStringLiteral("features"), mf);
        } else
            notes << tr("its mesh this import's: the model's lengths in %1, not %2").arg(c.value(QStringLiteral("unit")).toString(), unit);
        notes << tr("its Qucs-S line read back: the model's own %1 semiconductor features, its settings, parameters, functions%2")
                     .arg(n)
                     .arg(carriedMesh.isEmpty() ? QString() : tr(" and mesh"));
    }
    const double norm = solver.number(QStringLiteral("norm length"), 1e-6);
    QJsonObject physics{{QStringLiteral("type"), QStringLiteral("semiconductor")},
                        {QStringLiteral("tag"), QStringLiteral("semi")},
                        {QStringLiteral("T"), QStringLiteral("%1[K]").arg(num(T))},
                        {QStringLiteral("features"), features}};
    for (auto it = carriedSettings.begin(); it != carriedSettings.end(); ++it) physics.insert(it.key(), it.value());
    if (!metals.isEmpty()) physics.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}, {QStringLiteral("except"), only(metals)}});
    QJsonArray steps;
    if (!sweeps.isEmpty()) steps.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("sweep")}, {QStringLiteral("parameters"), sweeps}});
    steps.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("stationary")}});
    QJsonArray plots{QJsonObject{{QStringLiteral("type"), QStringLiteral("plotgroup")}, {QStringLiteral("label"), tr("Potential")},
                                 {QStringLiteral("plots"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("surface")}, {QStringLiteral("expression"), QStringLiteral("semi.V")}}}}},
                     QJsonObject{{QStringLiteral("type"), QStringLiteral("plotgroup")}, {QStringLiteral("label"), tr("Electrons")},
                                 {QStringLiteral("plots"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("surface")}, {QStringLiteral("expression"), QStringLiteral("log10(semi.n*1[cm^3])")}}}}}};
    if (!swept.isEmpty()) {
        // The swept contact's current - a gate's none: then those of the
        // others at a voltage (a MOSFET's drain), else of all the others.
        QStringList shown;
        const auto sweptContact = std::find_if(contacts.begin(), contacts.end(), [&](const Contact& c) { return c.name == swept; });
        if (sweptContact != contacts.end() && sweptContact->gate) {
            for (const Contact& c : contacts)
                if (!c.gate && c.o->number(QStringLiteral("voltage"), 0) != 0) shown << c.name;
            if (shown.isEmpty())
                for (const Contact& c : contacts)
                    if (!c.gate) shown << c.name;
        }
        if (shown.isEmpty()) shown << swept;
        QJsonArray graphs;
        for (const QString& c : shown)
            graphs.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("globalgraph")}, {QStringLiteral("expression"), QStringLiteral("semi.I_%1").arg(contactName(c))}});
        plots.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("plotgroup1d")}, {QStringLiteral("label"), tr("Current")}, {QStringLiteral("plots"), graphs}});
    }
    // Its mesh constraints: the boundaries within each one's box that fine.
    QJsonArray meshFeatures;
    for (const Object& o : scene.objects) {
        if (o.kind != QLatin1String("meshconstraint")) continue;
        const double h = o.number(QStringLiteral("max edge length"), NAN);
        if (!(h > 0)) continue;
        std::pair<double, double> b[3];
        for (int a = 0; a < 3; ++a) b[a] = span(o, axes[a], INFINITY);
        const double e = 1e-6 * size;
        const double x0 = std::max(b[p.u].first, u0 - 1) - e, x1 = std::min(b[p.u].second, u1 + 1) + e;
        const double y0 = std::max(b[p.v].first, v0 - 1) - e, y1 = std::min(b[p.v].second, v1 + 1) + e;
        meshFeatures.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("size")},
                                        {QStringLiteral("level"), QStringLiteral("boundary")},
                                        {QStringLiteral("label"), o.name()},
                                        {QStringLiteral("selection"), QJsonObject{{QStringLiteral("box"), QJsonArray{inUnit(x0), inUnit(y0), inUnit(x1), inUnit(y1)}}}},
                                        {QStringLiteral("hmax"), inUnit(h)}});
    }
    if (!meshFeatures.isEmpty()) notes << tr("its mesh constraints: the boundaries within each one's box meshed to its largest edge");
    // Lumerical refines its mesh where the doping changes, between its
    // smallest and largest edges: here the semiconductors no coarser than a
    // quarter of the thinnest or Lumerical's largest edge, each diffusion's
    // fall (its Gaussian's width σ) at its source face's boundaries σ/2.
    // (A Qucs-S export's own mesh carried: that one.)
    if (carriedMesh.isEmpty()) {
        const double maxEdge = solver.number(QStringLiteral("max edge length"), INFINITY), minEdge = solver.number(QStringLiteral("min edge length"), 0);
        double thinnest = INFINITY;
        QStringList semis;
        for (const Shape& sh : shapes)
            if (!dropped.contains(sh.label) && !isMetal(sh) && !insulator(sh)) {
                thinnest = std::min(thinnest, std::min(sh.x1 - sh.x0, sh.y1 - sh.y0));
                semis << sh.label;
            }
        const double h = std::max(minEdge, std::min(maxEdge, thinnest / 4));
        if (!semis.isEmpty() && std::isfinite(h) && h > 0) {
            meshFeatures.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("size")}, {QStringLiteral("level"), QStringLiteral("domain")},
                                            {QStringLiteral("label"), tr("Semiconductors")}, {QStringLiteral("selection"), only(semis)}, {QStringLiteral("hmax"), inUnit(h)}});
            int fine = 0;
            for (const Object& o : scene.objects) {
                if (o.kind != QLatin1String("diffusion")) continue;
                const double dj = o.number(QStringLiteral("junction width"), NAN), N0 = o.number(QStringLiteral("concentration"), NAN),
                             Nb = o.number(QStringLiteral("ref concentration"), NAN);
                if (!(dj > 0) || !(N0 > Nb) || !(Nb > 0)) continue;
                const double sigma = dj / std::sqrt(std::log(N0 / Nb));
                const double hd = std::max(minEdge, sigma / 2);
                if (!(hd < h)) continue;
                std::pair<double, double> b[3];
                for (int a = 0; a < 3; ++a) b[a] = span(o, axes[a], INFINITY);
                const double x0 = std::max(b[p.u].first, u0) - dj, x1 = std::min(b[p.u].second, u1) + dj;
                const double y0 = std::max(b[p.v].first, v0) - dj, y1 = std::min(b[p.v].second, v1) + dj;
                meshFeatures.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("size")},
                                                {QStringLiteral("level"), QStringLiteral("boundary")},
                                                {QStringLiteral("label"), tr("%1's fall").arg(o.name())},
                                                {QStringLiteral("selection"), QJsonObject{{QStringLiteral("box"), QJsonArray{inUnit(x0), inUnit(y0), inUnit(x1), inUnit(y1)}}}},
                                                {QStringLiteral("hmax"), inUnit(hd)}});
                ++fine;
            }
            notes << tr("its mesh: the semiconductors to %1 µm%2, %3 diffusion falls finer at their boundaries (σ/2)")
                         .arg(num(h * 1e6),
                              std::isfinite(maxEdge) ? tr(" (the Lumerical edges %1 to %2 µm)").arg(num(minEdge * 1e6), num(maxEdge * 1e6)) : QString())
                         .arg(fine);
        }
    }
    QJsonObject meshJson{{QStringLiteral("size"), QStringLiteral("fine")}};
    if (!meshFeatures.isEmpty()) meshJson.insert(QStringLiteral("features"), meshFeatures);
    if (!carriedMesh.isEmpty()) meshJson = carriedMesh;
    const QJsonObject component{{QStringLiteral("unit"), unit},
                                {QStringLiteral("thickness"), QStringLiteral("%1[um]").arg(num(norm * 1e6))},
                                {QStringLiteral("geometry"), geometry},
                                {QStringLiteral("materials"), materials},
                                {QStringLiteral("physics"), QJsonArray{physics}},
                                {QStringLiteral("mesh"), meshJson}};
    made.model = model(tr("%1 (Lumerical CHARGE)").arg(scene.source), tr("Imported from %1: its cross-section, %2 structures, %3 contacts.").arg(scene.source).arg(shapes.size() - dropped.size()).arg(contacts.size()),
                      component, QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("study")}, {QStringLiteral("label"), tr("Study 1")}, {QStringLiteral("steps"), steps}}},
                      QJsonObject{{QStringLiteral("plots"), plots}}, parameters);
    if (!functions.isEmpty()) made.model.insert(QStringLiteral("functions"), functions);
    made.notes = QStringList{tr("%1: a CHARGE cross-section (%2), %3 structures, %4 doping objects, %5 contacts, %6 K, norm length %7 µm")
                                .arg(scene.source, p.normal == 1 ? QStringLiteral("x-z") : p.normal == 0 ? QStringLiteral("y-z") : QStringLiteral("x-y"))
                                .arg(shapes.size() - dropped.size())
                                .arg(std::count_if(features.begin(), features.end(), [](const QJsonValue& v) { return v.toObject().value(QStringLiteral("type")) == QLatin1String("doping"); }))
                                .arg(contacts.size())
                                .arg(num(T), num(norm * 1e6))}
                + notes + scene.notes;
    return made;
    };
    LumericalImport result = assemble();
    // (A strip left out only where the boundaries in its box cover it - an
    // export's are each a whole boundary's: else it stays, a metal domain.)
    if (!dropped.isEmpty()) {
        std::optional<Model> m = Model::fromJson(QJsonDocument(result.model).toJson());
        const std::shared_ptr<ParameterScope> ps = m ? evaluateParameters(*m) : nullptr;
        const std::optional<GeometryBuild> g = m ? std::optional<GeometryBuild>(buildGeometry(*m, *ps)) : std::nullopt;
        bool again = false;
        for (const Contact& c : contacts) {
            if (c.strip.isEmpty() || !dropped.contains(c.strip)) continue;
            double covered = 0;
            if (g && g->ok()) {
                QString why;
                for (int bd : resolveSelection(*g->topology, *m, Level::Boundary, c.box, &why)) {
                    const std::vector<QPointF> pts = g->topology->boundaries[std::size_t(bd)].polyline(*g->topology);
                    for (std::size_t i = 1; i < pts.size(); ++i) covered += std::hypot(pts[i].x() - pts[i - 1].x(), pts[i].y() - pts[i - 1].y());
                }
            }
            if (covered < 0.99 * c.footprint) {
                dropped.remove(c.strip);
                again = true;
            }
        }
        if (again) result = assemble();
    }
    return result;
}

// --- HEAT -----------------------------------------------------------------

/// A HEAT project's cross-section as a heat model: its solids' thermal
/// conductivity (of T where it varies so), density and heat capacity, its
/// fluids not solved - convection on their boundaries -, its temperatures
/// (on the region's faces, between two materials, on a solid), its uniform
/// power heat sources (over the norm length, as Lumerical has them).
LumericalImport buildHeat(const Scene& scene, const Object& solver)
{
    LumericalImport out;
    QStringList notes;
    const Object* region = nullptr;
    const QString regionName = solver.text(QStringLiteral("simulation region"));
    for (const Object& o : scene.objects)
        if (o.kind == QLatin1String("region") && (regionName.isEmpty() || o.name() == regionName)) region = &o;
    // (Its plane as CHARGE's: a text, or a number - 1 x-normal, 2 y-normal, 3 z-normal.)
    Plane p;
    p.normal = 2;
    p.u = 0;
    p.v = 1;
    const QLatin1String axes[3] = {QLatin1String("x"), QLatin1String("y"), QLatin1String("z")};
    if (region) {
        QString dim = region->text(QStringLiteral("dimension")).toLower();
        if (const Value* d = region->prop(QStringLiteral("dimension")); d && d->isNumber())
            dim = QStringList{QStringLiteral("x-normal"), QStringLiteral("y-normal"), QStringLiteral("z-normal"), QStringLiteral("3d")}.value(whole(d->toNumber(3), 1, 4, 3) - 1, QStringLiteral("z-normal"));
        if (dim.contains(QLatin1String("3d"))) {
            out.error = tr("its simulation region is 3D: a 2D one's cross-section is read");
            return out;
        }
        if (dim.contains(QLatin1String("x-normal"))) {
            p.normal = 0;
            p.u = 1;
            p.v = 2;
        } else if (dim.contains(QLatin1String("y-normal"))) {
            p.normal = 1;
            p.u = 0;
            p.v = 2;
        }
        std::pair<double, double> b[3];
        for (int a = 0; a < 3; ++a) b[a] = span(*region, axes[a], a == p.normal ? 0 : INFINITY);
        p.at = std::isfinite(b[p.normal].first + b[p.normal].second) ? (b[p.normal].first + b[p.normal].second) / 2 : 0;
        p.u0 = b[p.u].first;
        p.u1 = b[p.u].second;
        p.v0 = b[p.v].first;
        p.v1 = b[p.v].second;
    }
    // Its materials' thermal records.
    struct Thermal {
        QJsonObject json;
        bool fluid = false;
        QString alias;
        QStringList notes;   // (said if it is used)
    };
    std::map<QString, Thermal> tm;
    for (const Object& o : scene.objects) {
        if (o.kind != QLatin1String("modelmaterial")) continue;
        Thermal t;
        t.alias = o.text(QStringLiteral("@alias"));
        t.fluid = o.text(QStringLiteral("@thermalkind")) == QLatin1String("Fluid");
        t.json = QJsonObject{{QStringLiteral("label"), o.name()}};
        const Value* all = o.prop(QStringLiteral("@thermal"));
        auto value = [&all, &t, &o](const QString& role, const QString& unit, bool ofT) -> QString {
            const Value* m = all ? all->field(role) : nullptr;
            const Value* a = m ? m->field(QStringLiteral("active")) : nullptr;
            const Value* ps = m ? m->field(QStringLiteral("params")) : nullptr;
            if (!a || !ps) return {};
            const QString kind = a->toText();
            auto param = [&](const char* k) {
                const Value* v = ps->field(QString::fromLatin1(k));
                return v ? v->toNumber(NAN) : NAN;
            };
            if (kind == QLatin1String("Constant") && std::isfinite(param("A0"))) return QStringLiteral("%1[%2]").arg(num(param("A0")), unit);
            if (ofT && kind.startsWith(QLatin1String("Universal")) && std::isfinite(param("A")))
                return QStringLiteral("%1[%2]*(T/300[K])^(%3)").arg(num(param("A")), unit, num(param("eta")));
            t.notes << tr("%1: its %2 of the Lumerical form %3: not read").arg(o.name(), role, kind);
            return {};
        };
        const QString k = value(QStringLiteral("thermal conductivity"), QStringLiteral("W/(m*K)"), true);
        const QString rho = value(QStringLiteral("mass density"), QStringLiteral("kg/m^3"), false);
        const QString cp = value(QStringLiteral("specific heat"), QStringLiteral("J/(kg*K)"), false);
        if (!k.isEmpty()) t.json.insert(QStringLiteral("k"), k);
        if (!rho.isEmpty()) t.json.insert(QStringLiteral("rho"), rho);
        if (!cp.isEmpty()) t.json.insert(QStringLiteral("Cp"), cp);
        if (k.isEmpty() && !t.fluid) {
            if (const QString ours = libraryOf(o.name(), true); !ours.isEmpty()) t.json = QJsonObject{{QStringLiteral("label"), o.name()}, {QStringLiteral("library"), ours}};
            else t.notes << tr("%1: no thermal conductivity read - set it").arg(o.name());
        }
        tm[o.name()] = t;
    }
    QSet<QString> used;
    auto order = [&](const Object& o) { return whole(o.number(QStringLiteral("mesh order"), 2), -1000000000, 1000000000, 2); };
    std::vector<Shape> shapes = structures(scene, p, order, used, notes);
    if (shapes.empty()) {
        out.error = tr("no structure in its simulation region");
        return out;
    }
    double u0 = INFINITY, v0 = INFINITY, u1 = -INFINITY, v1 = -INFINITY;
    for (const Shape& sh : shapes) {
        u0 = std::min(u0, sh.x0);
        v0 = std::min(v0, sh.y0);
        u1 = std::max(u1, sh.x1);
        v1 = std::max(v1, sh.y1);
    }
    const double size = std::max(u1 - u0, v1 - v0);
    const std::pair<QString, double> unitScale = unitFor(size);
    const QString unit = unitScale.first;
    const double scale = unitScale.second;
    auto inUnit = [&](double m) { return num(m / scale); };
    QJsonArray geometry, materials, features;
    QStringList fluids;
    std::map<QString, QStringList> labelsOf;
    QString last;
    QStringList lastOn;
    auto flush = [&]() {
        if (last.isEmpty()) return;
        QJsonObject m = tm.count(last) ? tm.at(last).json : QJsonObject{{QStringLiteral("label"), last}};
        m.insert(QStringLiteral("selection"), only(lastOn));
        materials.append(m);
    };
    for (const Shape& sh : shapes) {
        geometry.append(shapeJson(sh, scale));
        if (sh.material != last) {
            flush();
            last = sh.material;
            lastOn.clear();
        }
        lastOn << sh.label;
        if (!labelsOf.count(sh.material) && tm.count(sh.material)) notes << tm.at(sh.material).notes;
        labelsOf[sh.material] << sh.label;
        if (tm.count(sh.material) && !tm.at(sh.material).alias.isEmpty() && tm.at(sh.material).alias != sh.material) labelsOf[tm.at(sh.material).alias] << sh.label;
        if (tm.count(sh.material) && tm.at(sh.material).fluid) fluids << sh.label;
    }
    flush();
    geometry.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("form union")}});
    if (!fluids.isEmpty()) notes << tr("its fluids (%1) not solved: convection on their boundaries").arg(fluids.join(QStringLiteral(", ")));
    // A boundary condition's surfaces: the region's faces (4), between two
    // materials (6), a solid's (3).
    auto surfaces = [&size, &p, &u1, &u0, &v1, &v0, &inUnit, &labelsOf, &shapes](const Object& o, std::vector<QJsonObject>* out) {
        const int type = whole(o.number(QStringLiteral("surface type"), -1), -1000000000, 1000000000, -1);
        const double e = 1e-6 * size;
        if (type == 4) {
            std::vector<QJsonObject>& boxes = *out;
            const char* faces[6] = {"x min", "x max", "y min", "y max", "z min", "z max"};
            for (int k = 0; k < 6; ++k) {
                if (o.number(QString::fromLatin1(faces[k]), 0) == 0) continue;
                const int axis = k / 2;
                const bool high = k % 2;
                if (axis == p.normal) continue;
                const double at = axis == p.u ? (high ? u1 : u0) : (high ? v1 : v0);
                const QJsonArray box = axis == p.u ? QJsonArray{inUnit(at - e), inUnit(v0 - e), inUnit(at + e), inUnit(v1 + e)}
                                                   : QJsonArray{inUnit(u0 - e), inUnit(at - e), inUnit(u1 + e), inUnit(at + e)};
                boxes.push_back(QJsonObject{{QStringLiteral("box"), box}});
            }
            return !boxes.empty();
        }
        if (type == 6) {
            const QString a = o.text(QStringLiteral("material 1")), b = o.text(QStringLiteral("material 2"));
            if (!labelsOf.count(a) || !labelsOf.count(b)) return false;
            out->push_back(QJsonObject{{QStringLiteral("between"), QJsonArray{QJsonArray::fromStringList(labelsOf.at(a)), QJsonArray::fromStringList(labelsOf.at(b))}}});
            return true;
        }
        if (type == 3) {
            const QString solid = o.text(QStringLiteral("solid"));
            for (const Shape& sh : shapes)
                if (sh.name == solid) {
                    out->push_back(QJsonObject{{QStringLiteral("of"), QJsonArray{sh.label}}});
                    return true;
                }
        }
        return false;
    };
    const double norm = solver.number(QStringLiteral("norm length"), 1e-6);
    for (const Object& o : scene.objects) {
        std::vector<QJsonObject> sels;
        if (o.kind == QLatin1String("tbc")) {
            if (!surfaces(o, &sels)) {
                notes << tr("%1: its surfaces (type %2) not found here: left out").arg(o.name()).arg(o.number(QStringLiteral("surface type"), -1));
                continue;
            }
            for (const QJsonObject& sel : sels)
                features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("temperature")}, {QStringLiteral("label"), o.name()},
                                            {QStringLiteral("T0"), QStringLiteral("%1[K]").arg(num(o.number(QStringLiteral("temperature"), 300)))}, {QStringLiteral("selection"), sel}});
        } else if (o.kind == QLatin1String("convection")) {
            if (!surfaces(o, &sels)) {
                notes << tr("%1: its surfaces not found here: left out").arg(o.name());
                continue;
            }
            for (const QJsonObject& sel : sels)
                features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("flux")}, {QStringLiteral("label"), o.name()}, {QStringLiteral("kind"), QStringLiteral("convective")},
                                            {QStringLiteral("h"), QStringLiteral("%1[W/(m^2*K)]").arg(num(o.number(QStringLiteral("h convection"), 10)))},
                                            {QStringLiteral("Text"), QStringLiteral("%1[K]").arg(num(o.number(QStringLiteral("ambient temperature"), 300)))},
                                            {QStringLiteral("selection"), sel}});
            if (o.number(QStringLiteral("convection model"), 0) != 0) notes << tr("%1: its convection model not the constant h: h as given").arg(o.name());
        } else if (o.kind == QLatin1String("heatsource")) {
            const QString solid = o.text(QStringLiteral("volume solid"));
            QStringList on;
            for (const Shape& sh : shapes)
                if (sh.name == solid) on << sh.label;
            if (on.isEmpty()) {
                notes << tr("%1: its solid %2 is not in the region: left out").arg(o.name(), solid);
                continue;
            }
            // (Its power over the norm length - the model's thickness -, or
            // over its own equivalent length.)
            double P = o.number(QStringLiteral("total power"), 0);
            if (o.number(QStringLiteral("use solver norm length"), 1) == 0 && o.number(QStringLiteral("equivalent length"), 0) > 0)
                P *= norm / o.number(QStringLiteral("equivalent length"));
            features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("source")}, {QStringLiteral("label"), o.name()}, {QStringLiteral("kind"), QStringLiteral("power")},
                                        {QStringLiteral("P0"), QStringLiteral("%1[W]").arg(num(P))}, {QStringLiteral("selection"), only(on)}});
        } else if (o.kind == QLatin1String("heatflux")) {
            notes << tr("%1: a heat flux condition not read: left out").arg(o.name());
        }
    }
    QJsonObject physics{{QStringLiteral("type"), QStringLiteral("heat")}, {QStringLiteral("tag"), QStringLiteral("ht")}, {QStringLiteral("features"), features}};
    if (!fluids.isEmpty()) physics.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}, {QStringLiteral("except"), only(fluids)}});
    const QJsonObject component{{QStringLiteral("unit"), unit},
                                {QStringLiteral("thickness"), QStringLiteral("%1[um]").arg(num(norm * 1e6))},
                                {QStringLiteral("geometry"), geometry},
                                {QStringLiteral("materials"), materials},
                                {QStringLiteral("physics"), QJsonArray{physics}},
                                {QStringLiteral("mesh"), QJsonObject{{QStringLiteral("size"), QStringLiteral("fine")}}}};
    const QJsonArray plots{QJsonObject{{QStringLiteral("type"), QStringLiteral("plotgroup")}, {QStringLiteral("label"), tr("Temperature")},
                                       {QStringLiteral("plots"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("surface")}, {QStringLiteral("expression"), QStringLiteral("T")}}}}}};
    out.model = model(tr("%1 (Lumerical HEAT)").arg(scene.source), tr("Imported from %1: its cross-section, %2 structures.").arg(scene.source).arg(shapes.size()), component,
                      QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("study")}, {QStringLiteral("label"), tr("Study 1")},
                                             {QStringLiteral("steps"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("stationary")}}}}}},
                      QJsonObject{{QStringLiteral("plots"), plots}});
    out.notes = QStringList{tr("%1: a HEAT cross-section (%2), %3 structures, norm length %4 µm")
                                .arg(scene.source, p.normal == 1 ? QStringLiteral("x-z") : p.normal == 0 ? QStringLiteral("y-z") : QStringLiteral("x-y"))
                                .arg(shapes.size())
                                .arg(num(norm * 1e6))}
                + notes + scene.notes;
    return out;
}

// --- MQW ------------------------------------------------------------------

LumericalImport buildMqw(const Scene& scene)
{
    LumericalImport out;
    QStringList notes;
    const Value& st = scene.stack;
    const Value* length = st.field(QStringLiteral("length"));
    const Value* mats = st.field(QStringLiteral("material"));
    if (!length || length->kind != Value::Matrix || length->m.empty() || !mats || mats->kind != Value::Cell || mats->cell.size() < length->m.size()) {
        out.error = tr("its mqwindex stack has no layers (length and material)");
        return out;
    }
    const std::size_t n = length->m.size();
    const Value* strain = st.field(QStringLiteral("strain"));
    const double gamma = st.field(QStringLiteral("gamma")) ? st.field(QStringLiteral("gamma"))->toNumber(0.0132) : 0.0132;
    double nr = 3.5;
    if (const Value* ne = st.field(QStringLiteral("neff")); ne && ne->kind == Value::Matrix && ne->cols == 2 && ne->rows >= 1) nr = ne->m[std::size_t(ne->rows)];
    // The layers: rectangles along y (nm), each its material.
    QJsonArray geometry, features, materials;
    double y = 0, total = 0, gapLeast = INFINITY;
    std::vector<double> gaps(n, NAN);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = length->m[i] * 1e9;
        const QString name = QStringLiteral("layer%1").arg(i + 1);
        geometry.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("rectangle")},
                                    {QStringLiteral("label"), name},
                                    {QStringLiteral("position"), QJsonArray{QStringLiteral("0"), num(y)}},
                                    {QStringLiteral("size"), QJsonArray{QStringLiteral("10"), num(t)}}});
        y += t;
        total += t;
        const Value& m = mats->cell[i];
        const double exx = strain && strain->m.size() > i ? strain->m[i] : 0.0;
        if (const Value* db = m.field(QStringLiteral("database_material"))) {
            double cx = 0, cy = 0;
            const QString compound = compoundOf(db->toText(), m.field(QStringLiteral("x")) ? m.field(QStringLiteral("x"))->toNumber() : 0,
                                                m.field(QStringLiteral("y")) ? m.field(QStringLiteral("y"))->toNumber() : 0, &cx, &cy);
            if (compound.isEmpty()) {
                out.error = tr("layer %1: the Lumerical material %2 is not a compound here").arg(i + 1).arg(db->toText());
                return out;
            }
            materials.append(QJsonObject{{QStringLiteral("label"), QStringLiteral("%1 %2").arg(compound, num(cx))},
                                         {QStringLiteral("compound"), compound},
                                         {QStringLiteral("x"), num(cx)},
                                         {QStringLiteral("y"), num(cy)},
                                         {QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{name}}}}});
            if (exx != 0) notes << tr("layer %1: its strain %2 - here the substrate's lattice makes it (set the physics' substrate)").arg(i + 1).arg(num(exx));
            continue;
        }
        auto f = [&](const char* k, double otherwise = NAN) {
            const Value* v = m.field(QString::fromLatin1(k));
            return v ? v->toNumber(otherwise) : otherwise;
        };
        const double eg = f("eg"), vb = f("vb", 0), g1 = f("gamma1"), g2 = f("gamma2"), g3 = f("gamma3");
        if (!std::isfinite(eg) || !std::isfinite(g1) || !std::isfinite(g2) || !(g1 > 2 * g2)) {
            out.error = tr("layer %1: its material wants eg, gamma1 and gamma2 (gamma1 > 2 gamma2)").arg(i + 1);
            return out;
        }
        gaps[i] = eg;
        gapLeast = std::min(gapLeast, eg);
        QJsonObject qm{{QStringLiteral("type"), QStringLiteral("material")},
                       {QStringLiteral("label"), tr("layer %1's").arg(i + 1)},
                       {QStringLiteral("source"), QStringLiteral("user")},
                       {QStringLiteral("Ec"), num(vb + eg) + QStringLiteral("[eV]")},
                       {QStringLiteral("Ev"), num(vb) + QStringLiteral("[eV]")},
                       {QStringLiteral("me"), num(f("me", 0.067))},
                       {QStringLiteral("mhh"), num(1 / (g1 - 2 * g2))},
                       {QStringLiteral("mlh"), num(1 / (g1 + 2 * g2))},
                       {QStringLiteral("mhhp"), num(1 / (g1 + g2))},
                       {QStringLiteral("mlhp"), num(1 / (g1 - g2))},
                       {QStringLiteral("gamma3"), num(std::isfinite(g3) ? g3 : g2)},
                       {QStringLiteral("Ep"), num(f("ep", 25)) + QStringLiteral("[eV]")},
                       {QStringLiteral("nr"), num(nr)},
                       {QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{name}}}}};
        if (std::isfinite(f("eps"))) qm.insert(QStringLiteral("epsilonr"), num(f("eps")));
        if (exx != 0) {
            qm.insert(QStringLiteral("exx"), num(exx));
            qm.insert(QStringLiteral("ac"), num(f("ac", 0)) + QStringLiteral("[eV]"));
            qm.insert(QStringLiteral("av"), num(f("av", 0)) + QStringLiteral("[eV]"));
            qm.insert(QStringLiteral("b"), num(f("b", 0)) + QStringLiteral("[eV]"));
            qm.insert(QStringLiteral("C11"), num(f("c11", 1.221e11)) + QStringLiteral("[Pa]"));
            qm.insert(QStringLiteral("C12"), num(f("c12", 0.566e11)) + QStringLiteral("[Pa]"));
        }
        features.append(qm);
    }
    if (!materials.isEmpty() && !features.isEmpty()) notes << tr("its layers of the Lumerical database and of their own both: the database's here the library's compounds");
    // The wells: the least gap's; the density per them.
    double wells = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (std::isfinite(gaps[i]) && gaps[i] <= gapLeast + 1e-9) wells += length->m[i] * 1e9;
    const Value& pr = scene.params;
    auto par = [&](const char* k, double otherwise) {
        const Value* v = pr.field(QString::fromLatin1(k));
        return v ? v->toNumber(otherwise) : otherwise;
    };
    const double T = par("T", 300), cden = par("cden", 0);
    const double density = wells > 0 ? cden * total / wells : cden;
    // (Several densities: a sweep of N, per the wells.)
    std::vector<double> densities;
    if (const Value* c = pr.field(QStringLiteral("cden")); c && c->kind == Value::Matrix && c->m.size() > 1)
        for (double d : c->m) densities.push_back(wells > 0 ? d * total / wells : d);
    // The field: the potential's slope (the bands at -q V).
    double F = 0;
    if (const Value* V = pr.field(QStringLiteral("V")); V && V->kind == Value::Matrix && V->cols == 2 && V->rows >= 2) {
        const double z0 = V->m[0], z1 = V->m[std::size_t(V->rows - 1)];
        const double v0 = V->m[std::size_t(V->rows)], v1 = V->m[std::size_t(2 * V->rows - 1)];
        if (z1 > z0) F = (v0 - v1) / (z1 - z0);
        if (V->rows > 2) notes << tr("its potential of %1 points: its mean slope taken").arg(V->rows);
    }
    const Value& cf = scene.config;
    auto conf = [&](const char* k, double otherwise) {
        const Value* v = cf.field(QString::fromLatin1(k));
        return v ? v->toNumber(otherwise) : otherwise;
    };
    QJsonObject physics{{QStringLiteral("type"), QStringLiteral("quantum")},
                        {QStringLiteral("tag"), QStringLiteral("qw")},
                        {QStringLiteral("valence"), QStringLiteral("kp4")},
                        {QStringLiteral("T"), num(T) + QStringLiteral("[K]")},
                        {QStringLiteral("h1d"), num(conf("dz", 1e-10) * 1e9) + QStringLiteral("[nm]")},
                        {QStringLiteral("features"), features}};
    QJsonArray parameters;
    if (scene.fields.size() > 1) {
        parameters.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("F")}, {QStringLiteral("expression"), num(scene.fields.front()) + QStringLiteral("[V/m]")},
                                      {QStringLiteral("description"), tr("the field along the stack")}});
        physics.insert(QStringLiteral("F"), QStringLiteral("F"));
    } else if (F != 0)
        physics.insert(QStringLiteral("F"), num(F) + QStringLiteral("[V/m]"));
    // The spectrum: its photon frequencies as energies.
    QJsonArray steps, rows;
    if (scene.fields.size() > 1) {
        QStringList v;
        for (double x : scene.fields) v << num(x);
        rows.append(QJsonArray{QStringLiteral("F"), v.join(QStringLiteral(", ")), QStringLiteral("V/m")});
    }
    if (densities.size() > 1) {
        QStringList v;
        for (double x : densities) v << num(x);
        rows.append(QJsonArray{QStringLiteral("N"), v.join(QStringLiteral(", ")), QStringLiteral("1/m^3")});
        parameters.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("N")}, {QStringLiteral("expression"), num(densities.front()) + QStringLiteral("[1/m^3]")},
                                      {QStringLiteral("description"), tr("the carriers in the wells")}});
        if (rows.size() > 1) notes << tr("its fields and densities swept: here every combination");
    }
    if (!rows.isEmpty()) steps.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("sweep")}, {QStringLiteral("parameters"), rows}});
    QString energies;
    if (const Value* ph = pr.field(QStringLiteral("phfreq")); ph && ph->kind == Value::Matrix && !ph->m.empty()) {
        QStringList e;
        bool even = ph->m.size() > 2;
        const std::size_t m = ph->m.size();
        for (std::size_t i = 1; even && i < m; ++i)
            even = std::abs(ph->m[i] - (ph->m[0] + (ph->m[m - 1] - ph->m[0]) * double(i) / double(m - 1))) <= 1e-9 * std::abs(ph->m[m - 1] - ph->m[0]);
        if (even) energies = QStringLiteral("linspace(%1[eV], %2[eV], %3)").arg(num(H * ph->m.front() / Q), num(H * ph->m.back() / Q)).arg(m);
        else {
            for (double x : ph->m) e << QStringLiteral("%1[eV]").arg(num(H * x / Q));
            energies = e.join(QStringLiteral(", "));
        }
    } else {
        energies = QStringLiteral("linspace(%1[eV], %2[eV], 201)").arg(num(gapLeast - 0.05), num(gapLeast + 0.35));
        notes << tr("no photon frequencies: about the wells' gap");
    }
    QJsonObject spectrum{{QStringLiteral("type"), QStringLiteral("spectrum")},
                         {QStringLiteral("energies"), energies},
                         {QStringLiteral("linewidth"), num(gamma / 2) + QStringLiteral("[eV]")},
                         {QStringLiteral("lineshape"), QStringLiteral("lorentzian")},
                         {QStringLiteral("nr"), num(nr)}};
    if (densities.size() > 1) spectrum.insert(QStringLiteral("density"), QStringLiteral("N"));
    else if (density > 0) spectrum.insert(QStringLiteral("density"), num(density) + QStringLiteral("[1/m^3]"));
    if (conf("exciton", 0) != 0) spectrum.insert(QStringLiteral("excitons"), true);
    // (mqwindex's bands each kt: four of a spectrum's states, as an export writes them.)
    if (cf.field(QStringLiteral("numeigenvalues"))) spectrum.insert(QStringLiteral("states"), std::max(1, int(conf("numeigenvalues", 24)) / 4));
    steps.append(spectrum);
    QJsonArray studies{QJsonObject{{QStringLiteral("type"), QStringLiteral("study")}, {QStringLiteral("tag"), QStringLiteral("gain")},
                                   {QStringLiteral("label"), density > 0 ? tr("Gain") : tr("Absorption")}, {QStringLiteral("steps"), steps}}};
    if (const Value* kt = pr.field(QStringLiteral("kt")); kt && kt->kind == Value::Matrix && kt->m.size() > 1)
        studies.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("study")},
                                   {QStringLiteral("tag"), QStringLiteral("bands")},
                                   {QStringLiteral("label"), tr("Valence bands")},
                                   {QStringLiteral("steps"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("bands")},
                                                                                    {QStringLiteral("kmax"), num(*std::max_element(kt->m.begin(), kt->m.end())) + QStringLiteral("[1/m]")},
                                                                                    {QStringLiteral("points"), int(kt->m.size())},
                                                                                    {QStringLiteral("states"), std::max(1, int(conf("numeigenvalues", 8)) / 2)},
                                                                                    {QStringLiteral("bound"), true}}}}});
    const QJsonObject component{{QStringLiteral("unit"), QStringLiteral("nm")},
                                {QStringLiteral("geometry"), geometry},
                                {QStringLiteral("materials"), materials},
                                {QStringLiteral("physics"), QJsonArray{physics}},
                                {QStringLiteral("mesh"), QJsonObject{{QStringLiteral("size"), QStringLiteral("normal")}, {QStringLiteral("custom"), true}, {QStringLiteral("hmax"), QStringLiteral("2")}}}};
    QJsonArray plots{QJsonObject{{QStringLiteral("type"), QStringLiteral("plotgroup1d")},
                                 {QStringLiteral("label"), density > 0 ? tr("Gain") : tr("Absorption")},
                                 {QStringLiteral("study"), QStringLiteral("gain")},
                                 {QStringLiteral("plots"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("globalgraph")},
                                                                                  {QStringLiteral("expression"), density > 0 ? QStringLiteral("qw.gTE; qw.gTM") : QStringLiteral("qw.alphaTE; qw.alphaTM")}}}}}};
    // (Its bands, as the project's import draws them: each state found.)
    if (studies.size() > 1)
        plots.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("plotgroup1d")}, {QStringLiteral("label"), tr("Valence bands")}, {QStringLiteral("study"), QStringLiteral("bands")},
                                 {QStringLiteral("plots"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("globalgraph")},
                                                                                  {QStringLiteral("expression"), QStringLiteral("qw.Ev*")}}}}});
    const QJsonObject results{{QStringLiteral("plots"), plots}};
    out.model = model(tr("%1 (Lumerical MQW)").arg(scene.source), tr("Imported from %1: mqwindex's stack of %2 layers.").arg(scene.source).arg(n), component, studies, results, parameters);
    out.notes = QStringList{tr("%1: an mqwindex stack of %2 layers (%3 nm, its wells %4 nm), 4x4 k·p, %5 K%6; its γ %7 meV a full width: half of it; its density %8 cm⁻³ over the stack %9 cm⁻³ in the wells")
                                .arg(scene.source)
                                .arg(n)
                                .arg(num(total), num(wells), num(T), scene.fields.size() > 1 ? tr(", %1 fields").arg(scene.fields.size()) : F != 0 ? tr(", a field of %1 V/m").arg(num(F)) : QString(),
                                     num(gamma * 1e3), num(cden * 1e-6), num(density * 1e-6))}
                + notes + scene.notes;
    return out;
}

// --- MODE's project (.lms) --------------------------------------------------

struct Bytes {
    const QByteArray& b;
    qint64 p = 0;
    bool bad = false;
    quint32 u32()
    {
        if (p + 4 > b.size()) {
            bad = true;
            return 0;
        }
        quint32 v = 0;
        for (int i = 0; i < 4; ++i) v |= quint32(quint8(b[int(p) + i])) << (8 * i);
        p += 4;
        return v;
    }
    double f64()
    {
        if (p + 8 > b.size()) {
            bad = true;
            return 0;
        }
        double d;
        std::memcpy(&d, b.constData() + p, 8);
        p += 8;
        return d;
    }
    QString text()
    {
        const quint32 n = u32();
        if (bad || n > 1u << 20 || p + n > b.size()) {
            bad = true;
            return {};
        }
        const QString s = QString::fromUtf8(b.mid(int(p), int(n))).remove(QChar(0));
        p += n;
        return s;
    }
    /// A tagged value - a structure group's, a polygon's -: its tag (0 an
    /// int, 1 a number, 2 a text - its length 0xFFFFFFFF none -, 3 a
    /// matrix: its name, its elements, dimensions, whether real, its values
    /// - a complex one's real parts, then imaginary ones -), its value.
    struct Tagged {
        int tag = -1;
        double x = 0;   // (an int's, a number's)
        QString s;
        std::vector<double> m;
        std::vector<quint32> dims;
    };
    bool tagged(Tagged& v)
    {
        v = Tagged{};
        if (bad || p >= b.size()) {
            bad = true;
            return false;
        }
        v.tag = quint8(b[int(p)]);
        ++p;
        if (v.tag == 0)
            v.x = double(qint32(u32()));
        else if (v.tag == 1)
            v.x = f64();
        else if (v.tag == 2) {
            const quint32 n = u32();
            if (n != 0xFFFFFFFFu) {
                if (bad || n > 1u << 24 || p + n > b.size()) bad = true;
                else {
                    v.s = QString::fromUtf8(b.mid(int(p), int(n))).remove(QChar(0));
                    p += n;
                }
            }
        } else if (v.tag == 3) {
            const quint32 name = u32();
            if (bad || name > 256 || p + name > b.size()) bad = true;
            else {
                p += name;
                const quint32 count = u32(), nd = u32();
                if (nd > 8) bad = true;
                for (quint32 i = 0; i < nd && !bad; ++i) v.dims.push_back(u32());
                const quint32 real = u32();
                u32();
                const quint64 values = quint64(count) * (real == 1 ? 1 : 2);
                if (bad || values > (1u << 24) || p + qint64(values) * 8 > b.size()) bad = true;
                else {
                    v.m.resize(std::size_t(values));
                    for (double& x : v.m) x = f64();
                }
            }
        } else
            bad = true;
        return !bad;
    }
};

/// A structure's mesh order among its tagged values: after its rotations
/// and its display's opacity (a number), a flag - 0 its own order, 1 its
/// material's -, the order, then a matrix (a polygon's its place). \a own:
/// the flag (-1 none); \a after: where that matrix is.
int meshOrderOf(const std::vector<Bytes::Tagged>& v, std::size_t from, int* own, std::size_t* after = nullptr)
{
    for (std::size_t i = from; i + 3 < v.size(); ++i)
        if (v[i].tag == 1 && v[i + 1].tag == 0 && v[i + 2].tag == 0 && v[i + 3].tag == 3) {
            *own = v[i + 1].x == 0 ? 1 : 0;
            if (after) *after = i + 3;
            return int(v[i + 2].x);
        }
    *own = -1;
    if (after) *after = v.size();
    return 2;
}

/// Whether a structure's rotations (three axes, then their angles: tagged
/// values from \a from) turn it.
bool turnedBy(const std::vector<Bytes::Tagged>& v, std::size_t from)
{
    if (v.size() < from + 4 || v[from + 3].tag != 3) return false;
    for (double a : v[from + 3].m)
        if (a != 0) return true;
    return false;
}

} // namespace

Scene readModeProject(const QByteArray& b, const QString& source)
{
    Scene scene;
    scene.source = source;
    static const QByteArray head("Lumerical material data file version 2.0", 41);
    const qint64 at = b.indexOf(head);
    if (!b.startsWith("LUMERICAL") || at < 0) {
        scene.error = tr("%1 is not a Lumerical MODE project (.lms)").arg(source);
        return scene;
    }
    Bytes r{b, at + head.size()};
    // Its materials: their names, kinds and data, by their ids.
    std::map<QString, QString> byId;
    const quint32 count = r.u32();
    for (quint32 i = 0; i < count && !r.bad && i < 100000; ++i) {
        bool ok = false;
        qint64 used = 0;
        const QVariantMap d = properties(b, &ok, r.p, &used);
        if (!ok) {
            scene.error = tr("%1: its material %2 does not read").arg(source).arg(i + 1);
            return scene;
        }
        r.p += used;
        Material m;
        m.name = d.value(QStringLiteral("name")).toString();
        m.priority = d.value(QStringLiteral("priority"), 2).toInt();
        const int type = d.value(QStringLiteral("type")).toInt();
        if (type == 7) {
            m.type = QStringLiteral("sampled");
            for (const QVariant& x : d.value(QStringLiteral("frequency")).toList()) m.f.push_back(x.toDouble());
            for (const QVariant& x : d.value(QStringLiteral("permittivity")).toList()) m.epsRe.push_back(x.toDouble());
            for (const QVariant& x : d.value(QStringLiteral("permittivity@im")).toList()) m.epsIm.push_back(x.toDouble());
        } else if (type == 8)
            m.type = QStringLiteral("PEC");
        else if (type == 0) {
            m.type = QStringLiteral("dielectric");
            const QVariantList e = d.value(QStringLiteral("permittivity")).toList();
            if (e.size() == 9)
                for (int c = 0; c < 3; ++c) m.n[c] = std::sqrt(std::max(0.0, e[c * 4].toDouble()));
            m.anisotropic = std::abs(m.n[0] - m.n[1]) > 1e-12 || std::abs(m.n[0] - m.n[2]) > 1e-12;
        } else if (type == 6) {
            // Sellmeier's: n² = c0 + c1 λ²/(λ² - c2) + c3 λ²/(λ² - c4) +
            // c5 λ²/(λ² - c6), λ in µm; each coefficient diagonal.
            m.type = QStringLiteral("sellmeier");
            for (int c = 0; c < 7; ++c) {
                const QVariantList e = d.value(QStringLiteral("coefficients%1").arg(c)).toList();
                for (int a = 0; a < 3; ++a) m.sellmeier[std::size_t(a)][std::size_t(c)] = e.size() == 9 ? e[a * 4].toDouble() : 0;
            }
            m.anisotropic = m.sellmeier[0] != m.sellmeier[1] || m.sellmeier[0] != m.sellmeier[2];
        } else if (type == 13) {
            // Temperature dependent: another material's index, and dn/dT (T
            // - Tref) (diagonal); free carriers' terms (Soref's) besides.
            m.type = QStringLiteral("thermal");
            m.base = d.value(QStringLiteral("basematerial")).toString();
            const QVariantList dn = d.value(QStringLiteral("dnDt")).toList(), dk = d.value(QStringLiteral("dkDt")).toList(),
                                tr0 = d.value(QStringLiteral("tref")).toList();
            for (int a = 0; a < 3; ++a) m.dndT[a] = dn.size() == 9 ? dn[a * 4].toDouble() : 0;
            m.dkdT = dk.size() == 9 ? dk[0].toDouble() : 0;
            m.tref = tr0.size() == 9 ? tr0[0].toDouble() : 300;
            m.thermal = d.value(QStringLiteral("includeTemperatureEffects"), 1).toInt() != 0;
            for (const char* k : {"dnAn", "dnAp", "daAn", "daAp"}) {
                const QVariantList c = d.value(QString::fromLatin1(k)).toList();
                if (c.size() == 9 && c[0].toDouble() != 0) m.carriers = true;
            }
        } else
            m.type = tr("kind %1").arg(type);
        byId[d.value(QStringLiteral("materialuuid")).toString()] = m.name;
        scene.materials.push_back(m);
    }
    for (Material& m : scene.materials)
        if (m.type == QLatin1String("thermal")) m.base = byId.count(m.base) ? byId.at(m.base) : QString();
    // Its objects: a tree - a solver's (1001) its properties, a structure's
    // (1000) its shape and values, then its properties; each its children.
    // A rectangle's (6) and a pyramid's (9) values a box - its index, its
    // extent -, its index and name as texts, then tagged ones (rotations,
    // mesh order); a polygon's (11) and a structure group's (13) all tagged,
    // a group's then its script and its own properties. A group's children
    // the structures its script made, as it saved them: without their kind
    // and id, a polygon without a count of children.
    struct Place {
        double x = 0, y = 0, z = 0;
    };
    auto put = [](Object& o, const QString& k, const Value& v) {
        o.props[k] = v;
        o.history.emplace_back(k, v);
    };
    // (Its properties, after its values; where they start.)
    auto dictionary = [&b, &r](QVariantMap& d) -> qint64 {
        static const QByteArray first("\x0b\x00\x00\x00" "acf_custom\x00", 15);
        const qint64 dict = b.indexOf(first, r.p) - 4;
        if (r.bad || dict < r.p || dict - r.p > 4096) {
            r.bad = true;
            return -1;
        }
        bool ok = false;
        qint64 used = 0;
        d = properties(b, &ok, dict, &used);
        if (!ok) {
            r.bad = true;
            return -1;
        }
        r.p = dict + used;
        return dict;
    };
    auto material = [&](Object& o, const QVariantMap& d, const QString& index, const std::vector<Bytes::Tagged>& v, std::size_t from) {
        put(o, QStringLiteral("index"), Value::text(index));
        const QString id = d.value(QStringLiteral("materialuuid")).toString();
        put(o, QStringLiteral("material"), Value::text(byId.count(id) ? byId.at(id) : QStringLiteral("<Object defined dielectric>")));
        int own = -1;
        const int order = meshOrderOf(v, from, &own);
        if (own >= 0) {
            put(o, QStringLiteral("override mesh order from material database"), Value::number(own));
            put(o, QStringLiteral("mesh order"), Value::number(order));
        }
        if (turnedBy(v, from)) scene.notes << tr("%1: turned - its rotations not read: taken unturned").arg(o.name());
    };
    std::function<void(int, bool, bool, Place)> record = [&r, &dictionary, &scene, &b, &put, &material, &source, &record](int depth, bool owned, bool inner, Place at) {
        if (r.bad || depth > 32) {
            r.bad = true;
            return;
        }
        quint32 kind = 1000;
        if (!inner) {
            kind = r.u32();
            const quint32 n = r.u32();
            if (r.bad || n > 64) {
                r.bad = true;
                return;
            }
            r.p += n;   // (its id)
        }
        quint32 children = 0;
        bool solver = false;   // (its children its own: an FDE's region, its data)
        bool group = false;
        Place inside = at;     // (where its children's coordinates are from)
        if (kind == 1000) {
            const quint32 shape = r.u32();
            if (shape == 11 || shape == 13) {
                const quint32 nf = r.u32();
                if (r.bad || nf > 256) {
                    r.bad = true;
                    return;
                }
                std::vector<Bytes::Tagged> f(nf);
                for (Bytes::Tagged& v : f)
                    if (!r.tagged(v)) return;
                if (shape == 13) {
                    const quint32 more = r.u32();
                    if (r.bad || more > 4096) {
                        r.bad = true;
                        return;
                    }
                    Bytes::Tagged v;
                    for (quint32 i = 0; i < more; ++i)
                        if (!r.tagged(v)) return;
                }
                QVariantMap d;
                if (dictionary(d) < 0) return;
                if (shape == 13) {
                    // (Its rotations 0-3, name 14, place 15-17.)
                    children = r.u32();
                    group = true;
                    const QString name = f.size() > 17 ? f[14].s : QString();
                    if (f.size() > 17 && d.value(QStringLiteral("use_relative_coordinates"), 1).toInt() != 0)
                        inside = Place{at.x + f[15].x, at.y + f[16].x, at.z + f[17].x};
                    if (turnedBy(f, 0)) scene.notes << tr("%1: a structure group turned - its rotations not read: its structures taken unturned").arg(name);
                    if (depth > 0 && !owned)
                        scene.notes << tr("%1: a structure group - its structures as its script made them when the project was saved (the script not run)").arg(name);
                } else {
                    // A polygon: its index (1), its vertices (2: x and y in
                    // turn), z min and max (3, 4), its index as a text (5),
                    // its name (8), rotations (9-12), its mesh order, its
                    // place (x, y: the matrix after the order).
                    if (r.p + 4 <= b.size() && Bytes{b, r.p}.u32() == 0) r.u32();   // (a count of children, none)
                    const bool fits = f.size() > 12 && f[2].tag == 3 && f[2].dims.size() == 2 && f[2].dims[0] == 2 && f[2].dims[1] >= 3 &&
                                      f[2].m.size() >= std::size_t(2 * f[2].dims[1]) && f[3].tag == 1 && f[4].tag == 1 && f[8].tag == 2;
                    if (!fits) {
                        scene.notes << tr("%1: a polygon that does not read here: left out").arg(f.size() > 8 ? f[8].s : QString());
                    } else if (depth > 0 && !owned) {
                        Object o;
                        o.kind = QStringLiteral("poly");
                        put(o, QStringLiteral("name"), Value::text(f[8].s));
                        int own = -1;
                        std::size_t after = 0;
                        meshOrderOf(f, 9, &own, &after);
                        const bool placed = after < f.size() && f[after].m.size() == 2;
                        put(o, QStringLiteral("x"), Value::number(at.x + (placed ? f[after].m[0] : 0)));
                        put(o, QStringLiteral("y"), Value::number(at.y + (placed ? f[after].m[1] : 0)));
                        put(o, QStringLiteral("z min"), Value::number(at.z + f[3].x));
                        put(o, QStringLiteral("z max"), Value::number(at.z + f[4].x));
                        const int nv = int(f[2].dims[1]);
                        std::vector<double> xy(std::size_t(2 * nv));
                        for (int i = 0; i < nv; ++i) {
                            xy[std::size_t(i)] = f[2].m[std::size_t(2 * i)];
                            xy[std::size_t(nv + i)] = f[2].m[std::size_t(2 * i + 1)];
                        }
                        put(o, QStringLiteral("vertices"), Value::matrix(std::move(xy), nv, 2));
                        material(o, d, f[5].tag == 2 && !f[5].s.isEmpty() ? f[5].s : QString::number(f[1].x), f, 9);
                        scene.objects.push_back(o);
                    }
                }
            } else {
                r.u32();
                r.u32();
                const bool known = shape == 6 || shape == 9;
                double box[9] = {};
                QString index, name;
                if (known) {
                    for (int i = 0; i < (shape == 9 ? 9 : 7); ++i) box[i] = r.f64();
                    index = r.text();
                    r.u32();
                    r.u32();
                    name = r.text();
                }
                const qint64 from = r.p;
                QVariantMap d;
                const qint64 dict = dictionary(d);
                if (dict < 0) return;
                children = r.u32();
                std::vector<Bytes::Tagged> tail;
                {
                    // (Read no further than its properties.)
                    const QByteArray upTo = QByteArray::fromRawData(b.constData(), qsizetype(dict));
                    Bytes t{upTo, from};
                    Bytes::Tagged v;
                    while (t.p < dict && t.tagged(v)) tail.push_back(v);
                }
                const Place& o0 = at;
                if (!known) {
                    scene.notes << tr("a structure of a kind not read (%1): rectangles, pyramids, polygons and structure groups are").arg(shape);
                } else if (depth > 0 && !owned) {
                    Object o;
                    put(o, QStringLiteral("name"), Value::text(name));
                    if (shape == 6) {
                        // (Its box: index, y min, x min, y max, x max, z min, z max.)
                        o.kind = QStringLiteral("rect");
                        put(o, QStringLiteral("x min"), Value::number(o0.x + box[2]));
                        put(o, QStringLiteral("x max"), Value::number(o0.x + box[4]));
                        put(o, QStringLiteral("y min"), Value::number(o0.y + box[1]));
                        put(o, QStringLiteral("y max"), Value::number(o0.y + box[3]));
                        put(o, QStringLiteral("z min"), Value::number(o0.z + box[5]));
                        put(o, QStringLiteral("z max"), Value::number(o0.z + box[6]));
                    } else {
                        // (A pyramid's: index, x, y, its x and y spans at
                        // the bottom, at the top, z min, z max.)
                        o.kind = QStringLiteral("pyramid");
                        put(o, QStringLiteral("x"), Value::number(o0.x + box[1]));
                        put(o, QStringLiteral("y"), Value::number(o0.y + box[2]));
                        put(o, QStringLiteral("x span bottom"), Value::number(box[3]));
                        put(o, QStringLiteral("y span bottom"), Value::number(box[4]));
                        put(o, QStringLiteral("x span top"), Value::number(box[5]));
                        put(o, QStringLiteral("y span top"), Value::number(box[6]));
                        put(o, QStringLiteral("z min"), Value::number(o0.z + box[7]));
                        put(o, QStringLiteral("z max"), Value::number(o0.z + box[8]));
                    }
                    material(o, d, index, tail, 0);
                    scene.objects.push_back(o);
                }
            }
        } else if (kind == 1001) {
            bool ok = false;
            qint64 used = 0;
            const QVariantMap d = properties(b, &ok, r.p, &used);
            if (!ok) {
                r.bad = true;
                return;
            }
            r.p += used;
            r.u32();
            children = r.u32();
            const QString name = d.value(QStringLiteral("name")).toString().section(QStringLiteral("::"), -1);
            if (depth == 0 && !d.value(QStringLiteral("setupscript")).toString().trimmed().isEmpty())
                scene.notes << tr("its model's setup script not run: its objects as it set them when the project was saved");
            if (d.contains(QStringLiteral("MODEType"))) {
                solver = true;
                const int t = d.value(QStringLiteral("MODEType")).toInt();
                if (t != 2) {
                    scene.error = tr("%1: its FDE solver is not 2D Z normal (MODEType %2): only that one is read").arg(source).arg(t);
                    return;
                }
                Object o;
                o.kind = QStringLiteral("fde");
                auto set = [&](const QString& k, const Value& v) { put(o, k, v); };
                set(QStringLiteral("name"), Value::text(name));
                set(QStringLiteral("solver type"), Value::text(QStringLiteral("2D Z normal")));
                set(QStringLiteral("x"), Value::number(d.value(QStringLiteral("GUIx")).toDouble()));
                set(QStringLiteral("x span"), Value::number(d.value(QStringLiteral("GUIwidth")).toDouble()));
                set(QStringLiteral("y"), Value::number(d.value(QStringLiteral("GUIy")).toDouble()));
                set(QStringLiteral("y span"), Value::number(d.value(QStringLiteral("GUIheight")).toDouble()));
                set(QStringLiteral("z"), Value::number(0));
                set(QStringLiteral("dx"), Value::number(d.value(QStringLiteral("dx")).toDouble()));
                set(QStringLiteral("dy"), Value::number(d.value(QStringLiteral("dy")).toDouble()));
                set(QStringLiteral("background index"), Value::number(d.value(QStringLiteral("refractiveIndex"), 1.0).toDouble()));
                set(QStringLiteral("simulation temperature"), Value::number(d.value(QStringLiteral("simulationTemperature"), 300.0).toDouble()));
                const double f = d.value(QStringLiteral("modeFrequency")).toDouble();
                if (f > 0) set(QStringLiteral("analysis/wavelength"), Value::number(C0 / f));
                set(QStringLiteral("analysis/number of trial modes"), Value::number(d.value(QStringLiteral("numberOfModes"), 4).toDouble()));
                if (d.value(QStringLiteral("useMaxIndex"), 1).toInt() == 0) {
                    set(QStringLiteral("analysis/use max index"), Value::number(0));
                    set(QStringLiteral("analysis/n"), Value::number(d.value(QStringLiteral("mRefIndex")).toString().toDouble()));
                }
                const double stop = d.value(QStringLiteral("stopFrequency")).toDouble();
                const int points = d.value(QStringLiteral("numberOfPoints")).toInt();
                if (stop > 0 && f > 0 && std::abs(stop - f) > 1e-9 * f && points > 1) {
                    set(QStringLiteral("analysis/stop wavelength"), Value::number(C0 / stop));
                    set(QStringLiteral("analysis/number of points"), Value::number(points));
                }
                // (Its sides: PML where its pmlMODE flags one; its boundary
                // codes 1 metal - others not known.)
                const QVariantList pml = d.value(QStringLiteral("pmlMODE")).toList();
                const char* sides[4] = {"x min bc", "x max bc", "y min bc", "y max bc"};
                const char* codes[4] = {"xMinBC", "xMaxBC", "yMinBC", "yMaxBC"};
                for (int k = 0; k < 4; ++k) {
                    const int code = d.value(QString::fromLatin1(codes[k]), 1).toInt();
                    const bool isPml = pml.size() == 6 && pml[k].toInt() != 0;
                    set(QString::fromLatin1(sides[k]), Value::text(isPml ? QStringLiteral("PML") : QStringLiteral("Metal")));
                    if (!isPml && code != 1) scene.notes << tr("its %1: boundary code %2 not known - metal taken").arg(QLatin1String(sides[k])).arg(code);
                }
                scene.objects.push_back(o);
            } else if (name == QLatin1String("mesh")) {
                scene.notes << tr("%1: a mesh override - the model's mesh its own").arg(name);
            } else if (name == QLatin1String("temperature") && d.value(QStringLiteral("type")).toInt() == 3) {
                scene.notes << tr("%1: a temperature import - its data a script's (not saved with the project): the FDE's temperature taken").arg(name);
            }
        } else {
            r.bad = true;
            return;
        }
        for (quint32 i = 0; i < children && !r.bad && scene.error.isEmpty(); ++i) record(depth + 1, owned || solver, inner || group, inside);
    };
    record(0, false, false, Place{});
    if (r.bad && scene.error.isEmpty()) scene.error = tr("%1: its objects do not read (at byte %2)").arg(source).arg(r.p);
    return scene;
}

LumericalImport buildModel(const Scene& scene)
{
    LumericalImport out;
    if (!scene.error.isEmpty()) {
        out.error = scene.error;
        return out;
    }
    for (const Object& o : scene.objects)
        if (o.kind == QLatin1String("fde")) return buildMode(scene, o);
    for (const Object& o : scene.objects)
        if (o.kind == QLatin1String("charge")) return buildCharge(scene, o);
    for (const Object& o : scene.objects)
        if (o.kind == QLatin1String("heat")) return buildHeat(scene, o);
    if (scene.stack.kind != Value::Null) return buildMqw(scene);
    out.error = tr("%1 makes no FDE region, CHARGE solver or mqwindex stack").arg(scene.source);
    return out;
}

std::vector<MaterialEntry> libraryEntries(const Scene& scene, QStringList* notes)
{
    std::vector<MaterialEntry> out;
    QStringList alloys;
    const QString category = tr("Lumerical (%1)").arg(scene.source);
    auto put = [](MaterialEntry& e, const char* key, const QString& v) {
        if (!v.isEmpty()) e.properties.insert(QString::fromLatin1(key), v);
    };
    // Its electrical and thermal records (DEVICE's): semiconductors with
    // their models, insulators, conductors (their work functions).
    for (const Object& o : scene.objects) {
        if (o.kind != QLatin1String("modelmaterial")) continue;
        const QString kind = o.text(QStringLiteral("@kind"));
        if (kind.contains(QLatin1String("Alloy"))) {
            alloys << o.name();
            continue;
        }
        MaterialEntry e;
        e.name = o.name();
        e.category = category;
        e.user = true;
        e.description = tr("From %1: the Lumerical %2 record, its models' parameters of T as there").arg(scene.source, kind.isEmpty() ? tr("thermal") : kind.toLower());
        const Value* all = o.prop(QStringLiteral("@models"));
        auto model = [&](const QString& key) { return all && all->kind == Value::Struct ? all->field(key) : nullptr; };
        auto active = [&](const QString& key) {
            const Value* m = model(key);
            const Value* a = m ? m->field(QStringLiteral("active")) : nullptr;
            return a ? a->toText() : QString();
        };
        auto on = [&](const QString& key) {
            const Value* m = model(key);
            const Value* en = m ? m->field(QStringLiteral("enabled")) : nullptr;
            return m && (!en || en->toNumber(1) != 0);
        };
        auto param = [&](const QString& key, const char* p, double otherwise = NAN) {
            const Value* m = model(key);
            const Value* ps = m ? m->field(QStringLiteral("params")) : nullptr;
            const Value* v = ps ? ps->field(QString::fromLatin1(p)) : nullptr;
            return v ? v->toNumber(otherwise) : otherwise;
        };
        // A parameter at 300 K and its exponent, a constant's or the
        // universal temperature model's: "v[unit]*(T/300[K])^(η)".
        auto ofTemperature = [&](const QString& key, const QString& unit, double scale = 1) -> QString {
            const QString a = active(key);
            if (a == QLatin1String("Constant") && std::isfinite(param(key, "A0"))) return ofT(param(key, "A0") * scale, unit, 0);
            if (a.startsWith(QLatin1String("Universal")) && std::isfinite(param(key, "A"))) return ofT(param(key, "A") * scale, unit, param(key, "eta", 0));
            return {};
        };
        if (o.prop(QStringLiteral("@epsilonr"))) put(e, "epsilonr", num(o.number(QStringLiteral("@epsilonr"))));
        if (kind == QLatin1String("Conductor") && o.number(QStringLiteral("work function"), 0) > 0)
            put(e, "phi", QStringLiteral("%1[eV]").arg(num(o.number(QStringLiteral("work function")))));
        if (kind == QLatin1String("Semiconductor")) {
            // Its gap (Varshni's) and masses (y0 + α T + β T²), of T; Nc and Nv
            // of them; χ from its work function to the intrinsic level.
            QString eg;
            if (active(QStringLiteral("bandgap")) == QLatin1String("Constant")) eg = QStringLiteral("%1[eV]").arg(num(param(QStringLiteral("bandgap"), "A0")));
            else if (model(QStringLiteral("bandgap")))
                eg = QStringLiteral("(%1[eV] - %2[eV/K]*T^2/(T + %3[K]))")
                         .arg(num(param(QStringLiteral("bandgap"), "y0")), num(param(QStringLiteral("bandgap"), "alpha", 0)), num(param(QStringLiteral("bandgap"), "beta", 0)));
            auto mass = [&](const QString& key) -> QString {
                if (active(key) == QLatin1String("Constant")) return num(param(key, "A0"));
                if (!model(key)) return {};
                return QStringLiteral("(%1 + %2[1/K]*T + %3[1/K^2]*T^2)").arg(num(param(key, "y0")), num(param(key, "alpha", 0)), num(param(key, "beta", 0)));
            };
            const QString me = mass(QStringLiteral("mass n")), mh = mass(QStringLiteral("mass p"));
            const double W = o.number(QStringLiteral("work function"), NAN);
            if (eg.isEmpty() || me.isEmpty() || mh.isEmpty() || !std::isfinite(W)) {
                if (notes) *notes << tr("%1: its gap, masses or work function not read - left out").arg(o.name());
                continue;
            }
            put(e, "Eg", eg);
            put(e, "Nc", QStringLiteral("2.509e19[1/cm^3]*%1^1.5*(T/300[K])^1.5").arg(me));
            put(e, "Nv", QStringLiteral("2.509e19[1/cm^3]*%1^1.5*(T/300[K])^1.5").arg(mh));
            put(e, "chi", QStringLiteral("%1[eV] - %2/2 - 0.75*kB*T*log(%3/%4)").arg(num(W), eg, me, mh));
            put(e, "mun", ofTemperature(QStringLiteral("mobility lattice n"), QStringLiteral("cm^2/(V*s)")));
            put(e, "mup", ofTemperature(QStringLiteral("mobility lattice p"), QStringLiteral("cm^2/(V*s)")));
            for (const char* c : {"n", "p"}) {
                const QString C = QLatin1String(c), lattice = QStringLiteral("mobility lattice ") + C, impurity = QStringLiteral("mobility impurity ") + C;
                if (active(lattice).startsWith(QLatin1String("Universal")))
                    e.properties.insert(QStringLiteral("zeta") + C, num(-param(lattice, "eta", 0)));
                if (active(impurity) == QLatin1String("Caughey-Thomas") && param(impurity, "N300", 1e30) < 1e29) {
                    e.properties.insert(QStringLiteral("mu") + C + QStringLiteral("min"), ofT(param(impurity, "muMin300"), QStringLiteral("cm^2/(V*s)"), param(impurity, "eta1", 0)));
                    e.properties.insert(QStringLiteral("nref") + C, ofT(param(impurity, "N300"), QStringLiteral("1/cm^3"), param(impurity, "eta2", 0)));
                    e.properties.insert(QStringLiteral("alpha") + C, ofT(param(impurity, "p300"), QString(), param(impurity, "eta3", 0)));
                } else if (active(impurity) == QLatin1String("Masetti")) {
                    const QString mu = ofT(param(impurity, "muMin(300)"), QStringLiteral("cm^2/(V*s)"), param(impurity, "eta_muMin", 0));
                    e.properties.insert(QStringLiteral("m1") + C, mu);
                    e.properties.insert(QStringLiteral("m2") + C, mu);
                    e.properties.insert(QStringLiteral("mu1") + C, ofT(param(impurity, "mu2(300)"), QStringLiteral("cm^2/(V*s)"), param(impurity, "eta_mu2", 0)));
                    e.properties.insert(QStringLiteral("pc") + C, QStringLiteral("0[1/cm^3]"));
                    e.properties.insert(QStringLiteral("cr") + C, ofT(param(impurity, "Cr(300)"), QStringLiteral("1/cm^3"), param(impurity, "eta_Cr", 0)));
                    e.properties.insert(QStringLiteral("cs") + C, ofT(param(impurity, "Cs(300)"), QStringLiteral("1/cm^3"), param(impurity, "eta_Cs", 0)));
                    e.properties.insert(QStringLiteral("ma") + C, ofT(param(impurity, "alpha(300)"), QString(), param(impurity, "eta_alpha", 0)));
                    e.properties.insert(QStringLiteral("mb") + C, ofT(param(impurity, "beta(300)"), QString(), param(impurity, "eta_beta", 0)));
                }
                const QString vkey = QStringLiteral("vsat ") + C;
                const double v = active(vkey) == QLatin1String("Constant") ? param(vkey, "A0") : param(vkey, "vsat300");
                if (std::isfinite(v)) e.properties.insert(QStringLiteral("vsat") + C, ofT(v, QStringLiteral("cm/s"), 0));
                const double A = active(vkey) == QLatin1String("Constant") ? 0.0 : param(vkey, "Av");
                e.properties.insert(QStringLiteral("Av") + C, num(std::isfinite(A) ? A : 0.0));
                const double beta = param(QStringLiteral("high field ") + C, "beta");
                if (beta > 0) e.properties.insert(QStringLiteral("beta") + C, num(beta));
                e.properties.insert(QStringLiteral("tau") + C, ofTemperature(QStringLiteral("srh tau ") + C, QStringLiteral("s")));
                const QString doping = QStringLiteral("srh doping ") + C;
                if (active(doping) == QLatin1String("Fossum") && std::isfinite(param(doping, "N0")))
                    e.properties.insert(QStringLiteral("N0") + C, ofT(param(doping, "N0"), QStringLiteral("1/cm^3"), 0));
                const double mt = param(QStringLiteral("srh field ") + C, "mt");
                if (mt > 0) e.properties.insert(QStringLiteral("mt") + C, num(mt));
                e.properties.insert(QStringLiteral("cau") + C, ofTemperature(QStringLiteral("auger ") + C, QStringLiteral("cm^6/s")));
                const QString ii = QStringLiteral("impact ") + C;
                if (active(ii) == QLatin1String("Selberherr") && param(ii, "Ecrit(300)", 1e30) < 1e19) {
                    e.properties.insert(QStringLiteral("iia") + C, ofT(param(ii, "alphaInf(300)"), QStringLiteral("1/cm"), param(ii, "eta_alphaInf", 0)));
                    e.properties.insert(QStringLiteral("iib") + C, ofT(param(ii, "Ecrit(300)"), QStringLiteral("V/cm"), param(ii, "eta_Ecrit", 0)));
                    e.properties.insert(QStringLiteral("iibeta") + C, num(param(ii, "beta", 1)));
                }
            }
            if (active(QStringLiteral("srh doping n")) == QLatin1String("Fossum")) {
                put(e, "fA", num(param(QStringLiteral("srh doping n"), "alpha", 1)));
                put(e, "fB", num(param(QStringLiteral("srh doping n"), "beta", 0)));
                put(e, "fC", num(param(QStringLiteral("srh doping n"), "gamma", 1)));
                put(e, "fE", num(param(QStringLiteral("srh doping n"), "sigma", 1)));
            }
            double v1 = 0;
            for (const char* c : {"bgn n", "bgn p"})
                if (active(QLatin1String(c)) == QLatin1String("Slotboom")) v1 += param(QLatin1String(c), "V1", 0);
            if (v1 > 0) {
                put(e, "Ebgn", QStringLiteral("%1[eV]").arg(num(v1)));
                put(e, "Nbgn", QStringLiteral("%1[1/cm^3]").arg(num(param(QStringLiteral("bgn n"), "N0", 1e17))));
                put(e, "Cbgn", num(param(QStringLiteral("bgn n"), "C", 0.5)));
            }
            put(e, "Et", QStringLiteral("%1[eV]").arg(num(param(QStringLiteral("trap n"), "A0", 0))));
            put(e, "copt", ofTemperature(QStringLiteral("radiative n"), QStringLiteral("cm^3/s")));
            if (active(QStringLiteral("btbt")) == QLatin1String("Hurkx") && param(QStringLiteral("btbt"), "A", 0) > 0) {
                put(e, "bbtA", num(param(QStringLiteral("btbt"), "A")));
                put(e, "bbtB", QStringLiteral("%1[V/cm]").arg(num(param(QStringLiteral("btbt"), "B"))));
                put(e, "bbtgamma", num(param(QStringLiteral("btbt"), "eta", 2.5)));
            }
            Q_UNUSED(on);
        }
        // Its thermal record: κ, ρ, Cp (constant, or of T).
        if (const Value* th = o.prop(QStringLiteral("@thermal"))) {
            auto thermal = [&](const char* role, const QString& unit) -> QString {
                const Value* m = th->field(QString::fromLatin1(role));
                const Value* a = m ? m->field(QStringLiteral("active")) : nullptr;
                const Value* ps = m ? m->field(QStringLiteral("params")) : nullptr;
                if (!a || !ps) return {};
                auto p = [&](const char* k) {
                    const Value* v = ps->field(QString::fromLatin1(k));
                    return v ? v->toNumber(NAN) : NAN;
                };
                if (a->toText() == QLatin1String("Constant") && p("A0") > 0) return ofT(p("A0"), unit, 0);
                if (a->toText().startsWith(QLatin1String("Universal")) && p("A") > 0) return ofT(p("A"), unit, std::isfinite(p("eta")) ? p("eta") : 0.0);
                return {};
            };
            put(e, "k", thermal("thermal conductivity", QStringLiteral("W/(m*K)")));
            put(e, "rho", thermal("mass density", QStringLiteral("kg/m^3")));
            put(e, "Cp", thermal("specific heat", QStringLiteral("J/(kg*K)")));
        }
        if (e.properties.isEmpty()) continue;
        if (!e.properties.contains(QStringLiteral("mur"))) e.properties.insert(QStringLiteral("mur"), QStringLiteral("1"));
        out.push_back(e);
    }
    // Its optical materials (MODE's, FDTD's): their index of the wavelength.
    for (const Material& m : scene.materials) {
        MaterialEntry e;
        e.name = m.name;
        e.category = category;
        e.user = true;
        if (const std::optional<Dispersion> d = dispersionOf(m)) {
            // (Anisotropic: n along its axes, "nx; ny; nz" - a crystal's, its
            // orientation the library's material's.)
            e.properties.insert(QStringLiteral("n"), d->n[1] != d->n[0] || d->n[2] != d->n[0]
                                                         ? QStringLiteral("%1; %2; %3").arg(d->n[0], d->n[1], d->n[2])
                                                         : d->n[0]);
            if (!d->kappa.isEmpty()) e.properties.insert(QStringLiteral("kappa"), d->kappa);
            if (!d->range.isEmpty()) e.properties.insert(QStringLiteral("nrange"), d->range);
            e.description = tr("From %1: the Lumerical optical %2").arg(scene.source, d->what);
        } else if (m.type == QLatin1String("dielectric") || m.type.contains(QLatin1String("(n,k)"))) {
            auto axes = [](const double v[3]) {
                return v[1] != v[0] || v[2] != v[0] ? QStringLiteral("%1; %2; %3").arg(num(v[0]), num(v[1]), num(v[2])) : num(v[0]);
            };
            e.properties.insert(QStringLiteral("n"), axes(m.n));
            if (m.k[0] != 0 || m.k[1] != 0 || m.k[2] != 0) e.properties.insert(QStringLiteral("kappa"), axes(m.k));
            e.description = tr("From %1: a Lumerical optical (n,k) material, constant").arg(scene.source);
        } else
            continue;
        out.push_back(e);
    }
    if (notes && !alloys.isEmpty())
        *notes << tr("its alloys not taken (%1): the library's compounds are those, their composition each domain's").arg(alloys.join(QStringLiteral(", ")));
    return out;
}


} // namespace qucs_s::fem::lumerical
