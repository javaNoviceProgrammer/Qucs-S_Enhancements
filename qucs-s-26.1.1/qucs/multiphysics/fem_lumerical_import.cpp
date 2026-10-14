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
            axis = int(a->toNumber());
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
        if (o.kind != QLatin1String("rect") && o.kind != QLatin1String("poly") && o.kind != QLatin1String("circle")) continue;
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
    const int modes = analysis("number of trial modes") ? int(analysis("number of trial modes")->toNumber(4)) : 4;
    QString neff;
    if (analysis("search") && analysis("search")->toText().contains(QLatin1String("near")) && analysis("n")) neff = num(analysis("n")->toNumber(1));
    else if (analysis("use max index") && analysis("use max index")->toNumber(1) == 0 && analysis("n")) neff = num(analysis("n")->toNumber(1));
    // The structures, their materials.
    QSet<QString> used = {QStringLiteral("window")};
    auto order = [&](const Object& o) {
        if (o.number(QStringLiteral("override mesh order from material database"), 0) != 0) return int(o.number(QStringLiteral("mesh order"), 2));
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
    for (const Shape& s : shapes) {
        geometry.append(shapeJson(s, scale));
        QJsonObject m;
        const QString mat = s.material.trimmed();
        if (mat.isEmpty() || mat.startsWith(QLatin1Char('<'))) {
            // An object defined dielectric: its own index.
            bool ok = false;
            const double n = s.index.trimmed().toDouble(&ok);
            m = QJsonObject{{QStringLiteral("label"), tr("%1's index").arg(s.name)}, {QStringLiteral("n"), ok ? num(n) : QStringLiteral("1")}};
            if (!ok) notes << tr("%1: its index %2 not a number: 1 taken").arg(s.name, s.index);
        } else if (const Material* lm = materialNamed(scene, mat)) {
            if (lm->type == QLatin1String("PEC")) {
                m = QJsonObject{{QStringLiteral("label"), mat}, {QStringLiteral("n"), QStringLiteral("1")}, {QStringLiteral("kappa"), QStringLiteral("1e4")}};
                notes << tr("%1: a perfect conductor - an index with κ 1e4 (a PEC boundary on its edges is the exact one)").arg(mat);
            } else if (lm->anisotropic) {
                features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("wave")},
                                            {QStringLiteral("label"), mat},
                                            {QStringLiteral("source"), QStringLiteral("user")},
                                            {QStringLiteral("anisotropy"), QStringLiteral("diagonal")},
                                            {QStringLiteral("nx"), num(lm->n[0])},
                                            {QStringLiteral("ny"), num(lm->n[1])},
                                            {QStringLiteral("nz"), num(lm->n[2])},
                                            {QStringLiteral("kappa"), num(lm->k[0])},
                                            {QStringLiteral("selection"), only({s.label})}});
                m = QJsonObject{{QStringLiteral("label"), mat}, {QStringLiteral("n"), num(lm->n[0])}};
            } else if (const std::optional<std::array<double, 2>> nk = indexAt(*lm, C0 / lambda)) {
                m = QJsonObject{{QStringLiteral("label"), mat}, {QStringLiteral("n"), num((*nk)[0])}, {QStringLiteral("kappa"), num((*nk)[1])}};
                if (lm->type == QLatin1String("sampled")) {
                    const QString ours = libraryOf(mat, false);
                    const QString note = tr("%1: its index at %2 nm, n %3 κ %4 (Lumerical's data; constant%5)")
                                             .arg(mat, num(lambda * 1e9), QString::number((*nk)[0], 'f', 4), QString::number((*nk)[1], 'g', 3),
                                                  ours.isEmpty() ? QString() : tr(" - the library's %1 is dispersive").arg(ours));
                    if (!notes.contains(note)) notes << note;
                }
            } else {
                m = QJsonObject{{QStringLiteral("label"), mat}, {QStringLiteral("n"), QStringLiteral("1")}};
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
    if (!saidLibrary.isEmpty()) notes << tr("Lumerical's materials as the library's: %1").arg(saidLibrary.join(QStringLiteral(", ")));
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
        const int n = int(analysis("number of points")->toNumber(1));
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
            dim = QStringList{QStringLiteral("x-normal"), QStringLiteral("y-normal"), QStringLiteral("z-normal"), QStringLiteral("3d")}.value(int(d->toNumber(2)) - 1, QStringLiteral("y-normal"));
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
    // The model materials: Lumerical's electrical ones by name.
    struct ModelMaterial {
        QJsonObject json;
        bool metal = false, insulator = false;
        double workfunction = NAN;
        QStringList notes;   // (said if it is used)
    };
    std::map<QString, ModelMaterial> mm;
    for (const Object& o : scene.objects) {
        if (o.kind != QLatin1String("modelmaterial")) continue;
        ModelMaterial m;
        const QString name = o.name();
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
            } else
                m.notes << tr("%1: Lumerical's %2 not in the library - set its properties").arg(name, ct);
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
    auto order = [&](const Object& o) { return int(o.number(QStringLiteral("mesh order"), 2)); };
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
    auto assemble = [&pre, &mm, &shapes, &dropped, &scale, &isMetal, &insulator, &scene, &p, &size, &contacts, &solver, &u0, &u1, &v0, &v1, &unit]() -> LumericalImport {
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
    if (!library.isEmpty()) notes << tr("Lumerical's materials as the library's: %1").arg(library.join(QStringLiteral(", ")));
    if (!dropped.isEmpty()) notes << tr("the contacts' thin metal strips left out: each contact on the boundaries it covered");
    geometry.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("form union")}});
    // The doping.
    const QLatin1String axes[3] = {QLatin1String("x"), QLatin1String("y"), QLatin1String("z")};
    auto inUnit = [&](double m) { return num(m / scale); };
    for (const Object& o : scene.objects) {
        const bool dope = o.kind == QLatin1String("dope"), diffusion = o.kind == QLatin1String("diffusion");
        if (!dope && !diffusion && o.kind != QLatin1String("importdope")) continue;
        const QString name = o.name();
        const QString t = o.text(QStringLiteral("dopant type")).toLower();
        const QString dopant = t.startsWith(QLatin1Char('n')) || t == QLatin1String("0") || t.contains(QLatin1String("donor")) ? QStringLiteral("donor") : QStringLiteral("acceptor");
        if (o.kind == QLatin1String("importdope")) {
            notes << tr("%1: an imported doping profile (sampled) is not read - give it as an analytic doping").arg(name);
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
                                                  .arg(num(N0), inUnit(box.left()), inUnit(box.right()), inUnit(box.top()), inUnit(box.bottom())));
            }
        } else {
            // A diffusion: its plateau the box less the junction width on
            // each side but its source face's (out of the plane: every side).
            const double dj = o.number(QStringLiteral("junction width"), NAN), Nb = o.number(QStringLiteral("ref concentration"), NAN);
            if (!(dj > 0) || !(Nb > 0) || !(N0 > Nb)) {
                notes << tr("%1: its junction width and reference concentration: left out").arg(name);
                continue;
            }
            int face = int(o.number(QStringLiteral("source face"), 6));   // 1 x min ... 6 z max
            const int faceAxis = (face - 1) / 2, faceHigh = (face - 1) % 2;
            double x0 = box.left() + dj, x1 = box.right() - dj, y0 = box.top() + dj, y1 = box.bottom() - dj;
            if (faceAxis == p.u) (faceHigh ? x1 : x0) += faceHigh ? dj : -dj;
            if (faceAxis == p.v) (faceHigh ? y1 : y0) += faceHigh ? dj : -dj;
            // (No plateau - a profile from its source face - is a box of no depth.)
            if (!(x1 >= x0) || !(y1 >= y0)) {
                notes << tr("%1: its box narrower than its junction width: left out").arg(name);
                continue;
            }
            if (o.text(QStringLiteral("distribution function")).toLower().contains(QLatin1String("erf")))
                notes << tr("%1: Lumerical's erfc diffusion as a Gaussian").arg(name);
            f.insert(QStringLiteral("profile"), QStringLiteral("box"));
            f.insert(QStringLiteral("N0"), QStringLiteral("%1[1/m^3]").arg(num(N0)));
            f.insert(QStringLiteral("Nb"), QStringLiteral("%1[1/m^3]").arg(num(Nb)));
            f.insert(QStringLiteral("dj"), QStringLiteral("%1[um]").arg(num(dj * 1e6)));
            f.insert(QStringLiteral("position"), QJsonArray{inUnit(x0), inUnit(y0)});
            f.insert(QStringLiteral("size"), QJsonArray{inUnit(x1 - x0), inUnit(y1 - y0)});
        }
        features.append(f);
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
            const int n = std::max(1, int(o.number(QStringLiteral("range num points"), 1)));
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
    if (!semiconductors.isEmpty())
        features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("srh")}, {QStringLiteral("label"), tr("Recombination")}, {QStringLiteral("selection"), only(semiconductors)}});
    notes << tr("the materials' models (mobility, recombination) the library's, not Lumerical's: check them");
    const double T = solver.number(QStringLiteral("simulation temperature"), solver.number(QStringLiteral("temperature"), 300));
    const double norm = solver.number(QStringLiteral("norm length"), 1e-6);
    QJsonObject physics{{QStringLiteral("type"), QStringLiteral("semiconductor")},
                        {QStringLiteral("tag"), QStringLiteral("semi")},
                        {QStringLiteral("T"), QStringLiteral("%1[K]").arg(num(T))},
                        {QStringLiteral("features"), features}};
    if (!metals.isEmpty()) physics.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}, {QStringLiteral("except"), only(metals)}});
    QJsonArray steps;
    if (!sweeps.isEmpty()) steps.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("sweep")}, {QStringLiteral("parameters"), sweeps}});
    steps.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("stationary")}});
    QJsonArray plots{QJsonObject{{QStringLiteral("type"), QStringLiteral("plotgroup")}, {QStringLiteral("label"), tr("Potential")},
                                 {QStringLiteral("plots"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("surface")}, {QStringLiteral("expression"), QStringLiteral("semi.V")}}}}},
                     QJsonObject{{QStringLiteral("type"), QStringLiteral("plotgroup")}, {QStringLiteral("label"), tr("Electrons")},
                                 {QStringLiteral("plots"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("surface")}, {QStringLiteral("expression"), QStringLiteral("log10(semi.n*1[cm^3])")}}}}}};
    if (!swept.isEmpty())
        plots.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("plotgroup1d")}, {QStringLiteral("label"), tr("Current")},
                                 {QStringLiteral("plots"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("globalgraph")},
                                                                                  {QStringLiteral("expression"), QStringLiteral("semi.I_%1").arg(contactName(swept))}}}}});
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
    QJsonObject meshJson{{QStringLiteral("size"), QStringLiteral("fine")}};
    if (!meshFeatures.isEmpty()) meshJson.insert(QStringLiteral("features"), meshFeatures);
    const QJsonObject component{{QStringLiteral("unit"), unit},
                                {QStringLiteral("thickness"), QStringLiteral("%1[um]").arg(num(norm * 1e6))},
                                {QStringLiteral("geometry"), geometry},
                                {QStringLiteral("materials"), materials},
                                {QStringLiteral("physics"), QJsonArray{physics}},
                                {QStringLiteral("mesh"), meshJson}};
    made.model = model(tr("%1 (Lumerical CHARGE)").arg(scene.source), tr("Imported from %1: its cross-section, %2 structures, %3 contacts.").arg(scene.source).arg(shapes.size() - dropped.size()).arg(contacts.size()),
                      component, QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("study")}, {QStringLiteral("label"), tr("Study 1")}, {QStringLiteral("steps"), steps}}},
                      QJsonObject{{QStringLiteral("plots"), plots}}, parameters);
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
                out.error = tr("layer %1: Lumerical's %2 is not a compound here").arg(i + 1).arg(db->toText());
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
    if (!materials.isEmpty() && !features.isEmpty()) notes << tr("its layers of Lumerical's database and of their own both: the database's here the library's compounds");
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
                                                                                    {QStringLiteral("states"), std::max(1, int(conf("numeigenvalues", 8)) / 2)}}}}});
    const QJsonObject component{{QStringLiteral("unit"), QStringLiteral("nm")},
                                {QStringLiteral("geometry"), geometry},
                                {QStringLiteral("materials"), materials},
                                {QStringLiteral("physics"), QJsonArray{physics}},
                                {QStringLiteral("mesh"), QJsonObject{{QStringLiteral("size"), QStringLiteral("normal")}, {QStringLiteral("custom"), true}, {QStringLiteral("hmax"), QStringLiteral("2")}}}};
    const QJsonObject results{{QStringLiteral("plots"), QJsonArray{QJsonObject{
                                                         {QStringLiteral("type"), QStringLiteral("plotgroup1d")},
                                                         {QStringLiteral("label"), density > 0 ? tr("Gain") : tr("Absorption")},
                                                         {QStringLiteral("study"), QStringLiteral("gain")},
                                                         {QStringLiteral("plots"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("globalgraph")},
                                                                                                          {QStringLiteral("expression"), density > 0 ? QStringLiteral("qw.gTE; qw.gTM") : QStringLiteral("qw.alphaTE; qw.alphaTM")}}}}}}}};
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
};

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
        } else
            m.type = tr("kind %1").arg(type);
        byId[d.value(QStringLiteral("materialuuid")).toString()] = m.name;
        scene.materials.push_back(m);
    }
    // Its objects: a tree - a solver's (1001) its properties, a structure's
    // (1000) its box and index before them; each with its children.
    std::function<void(int, bool)> record = [&r, &b, &scene, &byId, &source, &record](int depth, bool owned) {
        if (r.bad || depth > 32) {
            r.bad = true;
            return;
        }
        const quint32 kind = r.u32();
        const quint32 n = r.u32();
        if (r.bad || n > 64) {
            r.bad = true;
            return;
        }
        r.p += n;   // (its id)
        quint32 children = 0;
        bool solver = false;   // (its children its own: an FDE's region, its data)
        if (kind == 1000) {
            const quint32 shape = r.u32();
            r.u32();
            r.u32();
            double box[7];
            for (double& x : box) x = r.f64();
            const QString index = r.text();
            r.u32();
            r.u32();
            const QString name = r.text();
            static const QByteArray first("\x0b\x00\x00\x00" "acf_custom\x00", 15);
            const qint64 dict = b.indexOf(first, r.p) - 4;
            if (r.bad || dict < r.p || dict - r.p > 4096) {
                r.bad = true;
                return;
            }
            bool ok = false;
            qint64 used = 0;
            const QVariantMap d = properties(b, &ok, dict, &used);
            if (!ok) {
                r.bad = true;
                return;
            }
            r.p = dict + used;
            children = r.u32();
            if (shape != 6) {
                scene.notes << tr("%1: a structure of a kind not read (%2): only rectangles are").arg(name).arg(shape);
            } else if (depth > 0 && !owned) {
                // (Its box: index, y min, x min, y max, x max, z min, z max.)
                Object o;
                o.kind = QStringLiteral("rect");
                auto put = [&](const QString& k, const Value& v) {
                    o.props[k] = v;
                    o.history.emplace_back(k, v);
                };
                put(QStringLiteral("name"), Value::text(name));
                put(QStringLiteral("x min"), Value::number(box[2]));
                put(QStringLiteral("x max"), Value::number(box[4]));
                put(QStringLiteral("y min"), Value::number(box[1]));
                put(QStringLiteral("y max"), Value::number(box[3]));
                put(QStringLiteral("z min"), Value::number(box[5]));
                put(QStringLiteral("z max"), Value::number(box[6]));
                put(QStringLiteral("index"), Value::text(index));
                const QString id = d.value(QStringLiteral("materialuuid")).toString();
                put(QStringLiteral("material"), Value::text(byId.count(id) ? byId.at(id) : QStringLiteral("<Object defined dielectric>")));
                scene.objects.push_back(o);
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
            if (d.contains(QStringLiteral("MODEType"))) {
                solver = true;
                const int t = d.value(QStringLiteral("MODEType")).toInt();
                if (t != 2) {
                    scene.error = tr("%1: its FDE solver is not 2D Z normal (MODEType %2): only that one is read").arg(source).arg(t);
                    return;
                }
                Object o;
                o.kind = QStringLiteral("fde");
                auto put = [&](const QString& k, const Value& v) {
                    o.props[k] = v;
                    o.history.emplace_back(k, v);
                };
                put(QStringLiteral("name"), Value::text(name));
                put(QStringLiteral("solver type"), Value::text(QStringLiteral("2D Z normal")));
                put(QStringLiteral("x"), Value::number(d.value(QStringLiteral("GUIx")).toDouble()));
                put(QStringLiteral("x span"), Value::number(d.value(QStringLiteral("GUIwidth")).toDouble()));
                put(QStringLiteral("y"), Value::number(d.value(QStringLiteral("GUIy")).toDouble()));
                put(QStringLiteral("y span"), Value::number(d.value(QStringLiteral("GUIheight")).toDouble()));
                put(QStringLiteral("z"), Value::number(0));
                put(QStringLiteral("dx"), Value::number(d.value(QStringLiteral("dx")).toDouble()));
                put(QStringLiteral("dy"), Value::number(d.value(QStringLiteral("dy")).toDouble()));
                put(QStringLiteral("background index"), Value::number(d.value(QStringLiteral("refractiveIndex"), 1.0).toDouble()));
                const double f = d.value(QStringLiteral("modeFrequency")).toDouble();
                if (f > 0) put(QStringLiteral("analysis/wavelength"), Value::number(C0 / f));
                put(QStringLiteral("analysis/number of trial modes"), Value::number(d.value(QStringLiteral("numberOfModes"), 4).toDouble()));
                if (d.value(QStringLiteral("useMaxIndex"), 1).toInt() == 0) {
                    put(QStringLiteral("analysis/use max index"), Value::number(0));
                    put(QStringLiteral("analysis/n"), Value::number(d.value(QStringLiteral("mRefIndex")).toString().toDouble()));
                }
                const double stop = d.value(QStringLiteral("stopFrequency")).toDouble();
                const int points = d.value(QStringLiteral("numberOfPoints")).toInt();
                if (stop > 0 && f > 0 && std::abs(stop - f) > 1e-9 * f && points > 1) {
                    put(QStringLiteral("analysis/stop wavelength"), Value::number(C0 / stop));
                    put(QStringLiteral("analysis/number of points"), Value::number(points));
                }
                // (Its sides: PML where its pmlMODE flags one; its boundary
                // codes 1 metal - others not known.)
                const QVariantList pml = d.value(QStringLiteral("pmlMODE")).toList();
                const char* sides[4] = {"x min bc", "x max bc", "y min bc", "y max bc"};
                const char* codes[4] = {"xMinBC", "xMaxBC", "yMinBC", "yMaxBC"};
                for (int k = 0; k < 4; ++k) {
                    const int code = d.value(QString::fromLatin1(codes[k]), 1).toInt();
                    const bool isPml = pml.size() == 6 && pml[k].toInt() != 0;
                    put(QString::fromLatin1(sides[k]), Value::text(isPml ? QStringLiteral("PML") : QStringLiteral("Metal")));
                    if (!isPml && code != 1) scene.notes << tr("its %1: boundary code %2 not known - metal taken").arg(QLatin1String(sides[k])).arg(code);
                }
                scene.objects.push_back(o);
            } else if (name == QLatin1String("mesh")) {
                scene.notes << tr("%1: a mesh override - the model's mesh its own").arg(name);
            }
        } else {
            r.bad = true;
            return;
        }
        for (quint32 i = 0; i < children && !r.bad && scene.error.isEmpty(); ++i) record(depth + 1, owned || solver);
    };
    record(0, false);
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
    if (scene.stack.kind != Value::Null) return buildMqw(scene);
    out.error = tr("%1 makes no FDE region, CHARGE solver or mqwindex stack").arg(scene.source);
    return out;
}

} // namespace qucs_s::fem::lumerical
