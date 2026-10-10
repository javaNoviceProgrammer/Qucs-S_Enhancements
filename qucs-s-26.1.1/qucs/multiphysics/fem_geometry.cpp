/*
 * fem_geometry.cpp - a multiphysics model's geometry: features, objects,
 *                    Form Union, selections
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_geometry.h"

#include <CDT.h>
#include <clipper/clipper.hpp>

#include <QCoreApplication>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <unordered_map>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Geometry", text);
}

double dot(QPointF a, QPointF b)
{
    return a.x() * b.x() + a.y() * b.y();
}

double cross(QPointF a, QPointF b)
{
    return a.x() * b.y() - a.y() * b.x();
}

double norm(QPointF a)
{
    return std::hypot(a.x(), a.y());
}

/// Distance from \a p to the segment a-b, and where along it (0 to 1).
double segmentDistance(QPointF p, QPointF a, QPointF b, double* along = nullptr)
{
    const QPointF d = b - a;
    const double len2 = dot(d, d);
    double u = len2 > 0 ? dot(p - a, d) / len2 : 0;
    if (along) *along = u;
    u = std::clamp(u, 0.0, 1.0);
    return norm(p - (a + d * u));
}

constexpr double ArcStep = M_PI / 180;   // an arc's pieces: a degree each, for the topology

} // namespace

// ------------------------------------------------------------------ Curves

QPointF Curve::at(double t) const
{
    if (kind == Line) return c + a * t;
    return c + a * std::cos(t) + b * std::sin(t);
}

double Curve::paramOf(QPointF p) const
{
    if (kind == Line) {
        const double len2 = dot(a, a);
        return len2 > 0 ? dot(p - c, a) / len2 : 0;
    }
    // [cos t, sin t] = M^-1 (p - c), M = [a b].
    const double det = cross(a, b);
    if (det == 0) return t0;
    const QPointF q = p - c;
    const double cs = (q.x() * b.y() - q.y() * b.x()) / det;
    const double sn = (a.x() * q.y() - a.y() * q.x()) / det;
    double t = std::atan2(sn, cs);
    // Into [t0, t1] (the arc may run either way, past ±pi).
    const double lo = std::min(t0, t1), hi = std::max(t0, t1);
    while (t < lo - 1e-9) t += 2 * M_PI;
    while (t > hi + 1e-9) t -= 2 * M_PI;
    if (t < lo - 1e-9) {
        // Beyond either end: the nearer one.
        const double dLo = std::abs(std::remainder(t - lo, 2 * M_PI)), dHi = std::abs(std::remainder(t - hi, 2 * M_PI));
        t = dLo < dHi ? lo : hi;
    }
    return t;
}

double Curve::radiusAt(double t) const
{
    if (kind == Line) return std::numeric_limits<double>::infinity();
    const QPointF d1 = -a * std::sin(t) + b * std::cos(t);
    const QPointF d2 = -a * std::cos(t) - b * std::sin(t);
    const double k = std::abs(cross(d1, d2));
    const double s = norm(d1);
    return k > 0 ? s * s * s / k : std::numeric_limits<double>::infinity();
}

double Curve::length() const
{
    if (kind == Line) return norm(a) * std::abs(t1 - t0);
    const int n = std::max(8, int(std::ceil(std::abs(t1 - t0) / ArcStep)) * 4);
    double len = 0;
    QPointF prev = at(t0);
    for (int i = 1; i <= n; ++i) {
        const QPointF p = at(t0 + (t1 - t0) * i / n);
        len += norm(p - prev);
        prev = p;
    }
    return len;
}

// ------------------------------------------------------------------ Objects

QRectF GObject::bounds() const
{
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    auto grow = [&](QPointF p) {
        x0 = std::min(x0, p.x());
        y0 = std::min(y0, p.y());
        x1 = std::max(x1, p.x());
        y1 = std::max(y1, p.y());
    };
    for (const auto& loop : loops)
        for (const GEdge& e : loop) grow(e.a);
    for (const GEdge& e : open) {
        grow(e.a);
        grow(e.b);
    }
    for (QPointF p : points) grow(p);
    if (x0 > x1) return {};
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

bool GObject::contains(QPointF p) const
{
    int winding = 0;
    for (const auto& loop : loops)
        for (const GEdge& e : loop) {
            if (e.a.y() <= p.y()) {
                if (e.b.y() > p.y() && cross(e.b - e.a, p - e.a) > 0) ++winding;
            } else if (e.b.y() <= p.y() && cross(e.b - e.a, p - e.a) < 0) {
                --winding;
            }
        }
    return winding != 0;
}

// ------------------------------------------------------------------ Topology

std::vector<QPointF> Topology::Boundary::polyline(const Topology& t) const
{
    std::vector<QPointF> pts;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const Segment& s = t.segments[std::size_t(segments[i])];
        const int a = reversed[i] ? s.v1 : s.v0, b = reversed[i] ? s.v0 : s.v1;
        if (i == 0) pts.push_back(t.vertices[std::size_t(a)]);
        pts.push_back(t.vertices[std::size_t(b)]);
    }
    return pts;
}

int Topology::domainAt(QPointF p) const
{
    for (int d = 0; d < int(domains.size()); ++d) {
        const Domain& dom = domains[std::size_t(d)];
        if (!dom.bounds.adjusted(-1e-12, -1e-12, 1e-12, 1e-12).contains(p)) continue;
        for (const auto& tri : dom.triangles) {
            const QPointF a = vertices[std::size_t(tri[0])], b = vertices[std::size_t(tri[1])], c = vertices[std::size_t(tri[2])];
            const double d1 = cross(b - a, p - a), d2 = cross(c - b, p - b), d3 = cross(a - c, p - c);
            if (d1 >= 0 && d2 >= 0 && d3 >= 0) return d;
        }
    }
    return -1;
}

int Topology::boundaryAt(QPointF p, double tolerance) const
{
    int best = -1;
    double bestDist = tolerance;
    for (int b = 0; b < int(boundaries.size()); ++b)
        for (int s : boundaries[std::size_t(b)].segments) {
            const Segment& seg = segments[std::size_t(s)];
            const double d = segmentDistance(p, vertices[std::size_t(seg.v0)], vertices[std::size_t(seg.v1)]);
            if (d <= bestDist) {
                bestDist = d;
                best = b;
            }
        }
    return best;
}

int Topology::pointAt(QPointF p, double tolerance) const
{
    int best = -1;
    double bestDist = tolerance;
    for (int i = 0; i < int(points.size()); ++i) {
        const double d = norm(vertices[std::size_t(points[std::size_t(i)].vertex)] - p);
        if (d <= bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

QPointF Topology::normalAt(int boundary, int domain) const
{
    const Boundary& b = boundaries[std::size_t(boundary)];
    const std::size_t mid = b.segments.size() / 2;
    const Segment& s = segments[std::size_t(b.segments[mid])];
    QPointF t = vertices[std::size_t(s.v1)] - vertices[std::size_t(s.v0)];
    if (b.reversed[mid]) t = -t;
    const double n = norm(t);
    if (n == 0) return {};
    t /= n;
    // The domain on the left (up): the outward normal is to the right.
    const QPointF right(t.y(), -t.x());
    return domain == b.up ? right : -right;
}

// ------------------------------------------------------------------ Building

namespace {

/// An affine map: x' = m11 x + m12 y + dx, y' = m21 x + m22 y + dy.
struct Affine {
    double m11 = 1, m12 = 0, m21 = 0, m22 = 1, dx = 0, dy = 0;
    QPointF map(QPointF p) const { return {m11 * p.x() + m12 * p.y() + dx, m21 * p.x() + m22 * p.y() + dy}; }
    QPointF vec(QPointF v) const { return {m11 * v.x() + m12 * v.y(), m21 * v.x() + m22 * v.y()}; }
    double det() const { return m11 * m22 - m12 * m21; }
    static Affine translate(double x, double y)
    {
        Affine a;
        a.dx = x;
        a.dy = y;
        return a;
    }
    static Affine rotate(double radians, QPointF about)
    {
        Affine a;
        const double c = std::cos(radians), s = std::sin(radians);
        a.m11 = c;
        a.m12 = -s;
        a.m21 = s;
        a.m22 = c;
        a.dx = about.x() - (c * about.x() - s * about.y());
        a.dy = about.y() - (s * about.x() + c * about.y());
        return a;
    }
};

class Builder
{
public:
    Builder(const Model& model, const ParameterScope& parameters) : a_model(model), a_ps(parameters)
    {
        const QString unit = model.component().text(QStringLiteral("unit"));
        const std::optional<Unit> u = parseUnit(unit);
        a_scale = u && u->dim == Dim::length() ? u->scale : 1e-3;
        a_unitName = unit;
    }

    GeometryBuild build(const QString& upTo)
    {
        GeometryBuild out;
        double tolerance = 1e-6;
        for (const Node& f : a_model.geometry().children) {
            if (!f.enabled) continue;
            a_error.clear();
            a_warning.clear();
            a_tag = f.tag;
            if (f.type == QLatin1String("form union")) {
                tolerance = number(f.text(QStringLiteral("tolerance")), 1e-6);
                if (!a_error.isEmpty() || tolerance <= 0) tolerance = 1e-6;
                break;
            }
            feature(f);
            if (!a_warning.isEmpty()) out.warnings.insert(f.tag, a_warning);
            if (!a_error.isEmpty()) {
                out.errors.insert(f.tag, a_error);
                out.failedAt = f.tag;
                break;
            }
            if (!upTo.isEmpty() && f.tag == upTo) break;
        }
        QString why;
        out.topology = formUnion(tolerance, &why);
        if (!out.topology) {
            const QString tag = a_model.geometry().children.back().tag;
            out.errors.insert(tag, why);
            if (out.failedAt.isEmpty()) out.failedAt = tag;
        } else if (!why.isEmpty()) {
            out.warnings.insert(a_model.geometry().children.back().tag, why);
        }
        return out;
    }

private:
    // --- Values ---------------------------------------------------------
    double evaluate(const QString& text, const Dim& expected, bool* isPlain)
    {
        // In the geometry's unit: a parameter of 2[mm] is 2 in a geometry
        // in mm, and "L/2 - 0.6" half of it less 0.6 mm.
        Expression::Options options;
        options.lengthUnit = a_scale;
        options.numbersAreLengths = true;
        const Expression e = Expression::compile(text, a_ps.scope, options);
        if (!e.isValid()) {
            fail(tr("%1: %2").arg(text, e.error()));
            return 0;
        }
        if (!e.isConstant()) {
            fail(tr("%1 must be a constant").arg(text));
            return 0;
        }
        if (!e.warnings().isEmpty()) warn(e.warnings().join(QLatin1String("; ")));
        if (!e.dim().isNone() && !(e.dim() == expected))
            warn(tr("%1 is in %2, where %3 is expected").arg(text, dimName(e.dim()), dimName(expected)));
        if (isPlain) *isPlain = e.dim().isNone();
        if (!std::isfinite(e.constant())) fail(tr("%1 is not a finite number").arg(text));
        return e.constant();
    }
    /// A length in the geometry's unit: a plain number is in it.
    double length(const QString& text)
    {
        bool plain = true;
        return evaluate(text, Dim::length(), &plain);
    }
    /// An angle, in radians: a plain number is in degrees.
    double angle(const QString& text)
    {
        bool plain = true;
        const double v = evaluate(text, Dim(), &plain);
        if (text.contains(QLatin1String("[rad]")) || text.contains(QLatin1String("[deg]"))) return v;
        return v * M_PI / 180;
    }
    double number(const QString& text, double fallback = 0)
    {
        if (text.trimmed().isEmpty()) return fallback;
        bool plain = true;
        return evaluate(text, Dim(), &plain);
    }
    QPointF lengths(const QStringList& pair) { return {length(pair.value(0)), length(pair.value(1))}; }
    void fail(const QString& why)
    {
        if (a_error.isEmpty()) a_error = why;
    }
    void warn(const QString& what)
    {
        if (!a_warning.contains(what)) a_warning += (a_warning.isEmpty() ? QString() : QStringLiteral("; ")) + what;
    }

    // --- Curves ---------------------------------------------------------
    int addCurve(const Curve& c)
    {
        a_curves.push_back(c);
        return int(a_curves.size()) - 1;
    }
    /// A curve in pieces: one for a line, a degree each for an arc.
    std::vector<GEdge> pieces(int curve) const
    {
        const Curve& c = a_curves[std::size_t(curve)];
        const int n = c.kind == Curve::Line ? 1 : std::max(1, int(std::ceil(std::abs(c.t1 - c.t0) / ArcStep - 1e-9)));
        std::vector<GEdge> out;
        QPointF prev = c.at(c.t0);
        for (int i = 1; i <= n; ++i) {
            const double t = c.t0 + (c.t1 - c.t0) * i / n;
            const QPointF p = i == n ? c.at(c.t1) : c.at(t);
            out.push_back({prev, p, curve, c.t0 + (c.t1 - c.t0) * (i - 1) / n, t});
            prev = p;
        }
        return out;
    }
    std::vector<GEdge> lineLoop(const std::vector<QPointF>& corners)
    {
        std::vector<GEdge> loop;
        for (std::size_t i = 0; i < corners.size(); ++i) {
            const QPointF a = corners[i], b = corners[(i + 1) % corners.size()];
            if (a == b) continue;
            Curve c;
            c.c = a;
            c.a = b - a;
            const int id = addCurve(c);
            loop.push_back({a, b, id, 0, 1});
        }
        return loop;
    }
    static double signedArea(const std::vector<GEdge>& loop)
    {
        double s = 0;
        for (const GEdge& e : loop) s += cross(e.a, e.b);
        return s / 2;
    }
    static void reverse(std::vector<GEdge>& loop)
    {
        std::reverse(loop.begin(), loop.end());
        for (GEdge& e : loop) {
            std::swap(e.a, e.b);
            std::swap(e.ta, e.tb);
        }
    }

    // --- Features -------------------------------------------------------
    void feature(const Node& f)
    {
        const QString& t = f.type;
        if (t == QLatin1String("rectangle")) rectangle(f);
        else if (t == QLatin1String("circle")) circle(f);
        else if (t == QLatin1String("ellipse")) ellipse(f);
        else if (t == QLatin1String("polygon")) polygon(f);
        else if (t == QLatin1String("point")) point(f);
        else if (t == QLatin1String("move") || t == QLatin1String("rotate") || t == QLatin1String("scale")
                 || t == QLatin1String("mirror") || t == QLatin1String("array"))
            transform(f);
        else if (t == QLatin1String("union") || t == QLatin1String("difference") || t == QLatin1String("intersection"))
            boolean(f);
        else fail(tr("%1 is not a geometry feature").arg(t));
    }
    void addObject(GObject o, const Node& f)
    {
        o.name = f.label.isEmpty() ? f.tag : f.label;
        a_objects.push_back(std::move(o));
    }
    void rectangle(const Node& f)
    {
        const QPointF pos = lengths(f.pair(QStringLiteral("position")));
        const QPointF size = lengths(f.pair(QStringLiteral("size")));
        const double rot = angle(f.text(QStringLiteral("rotation")));
        if (!a_error.isEmpty()) return;
        if (!(size.x() > 0) || !(size.y() > 0)) return fail(tr("a rectangle's width and height must be more than 0"));
        const bool centered = f.text(QStringLiteral("base")) == QLatin1String("center");
        const QPointF origin = centered ? pos - size / 2 : pos;
        std::vector<QPointF> corners = {origin, origin + QPointF(size.x(), 0), origin + size, origin + QPointF(0, size.y())};
        if (rot != 0) {
            const Affine r = Affine::rotate(rot, pos);
            for (QPointF& c : corners) c = r.map(c);
        }
        GObject o;
        o.loops.push_back(lineLoop(corners));
        addObject(std::move(o), f);
    }
    void arcObject(const Node& f, QPointF center, QPointF a, QPointF b, double sector, double rot)
    {
        GObject o;
        std::vector<GEdge> loop;
        if (sector >= 2 * M_PI - 1e-12) {
            // Four quarters, as COMSOL's circle has.
            for (int q = 0; q < 4; ++q) {
                Curve c;
                c.kind = Curve::Arc;
                c.c = center;
                c.a = a;
                c.b = b;
                c.t0 = rot + q * M_PI / 2;
                c.t1 = rot + (q + 1) * M_PI / 2;
                const int id = addCurve(c);
                for (const GEdge& e : pieces(id)) loop.push_back(e);
            }
        } else {
            Curve c;
            c.kind = Curve::Arc;
            c.c = center;
            c.a = a;
            c.b = b;
            c.t0 = rot;
            c.t1 = rot + sector;
            const int id = addCurve(c);
            const QPointF start = c.at(c.t0), end = c.at(c.t1);
            std::vector<GEdge> arc = pieces(id);
            Curve l1;
            l1.c = center;
            l1.a = start - center;
            const int i1 = addCurve(l1);
            loop.push_back({center, start, i1, 0, 1});
            for (const GEdge& e : arc) loop.push_back(e);
            Curve l2;
            l2.c = end;
            l2.a = center - end;
            const int i2 = addCurve(l2);
            loop.push_back({end, center, i2, 0, 1});
        }
        if (signedArea(loop) < 0) reverse(loop);
        o.loops.push_back(loop);
        addObject(std::move(o), f);
    }
    void circle(const Node& f)
    {
        const QPointF center = lengths(f.pair(QStringLiteral("center")));
        const double r = length(f.text(QStringLiteral("radius")));
        const double sector = angle(f.text(QStringLiteral("sector")));
        const double rot = angle(f.text(QStringLiteral("rotation")));
        if (!a_error.isEmpty()) return;
        if (!(r > 0)) return fail(tr("a circle's radius must be more than 0"));
        if (!(sector > 0)) return fail(tr("a sector's angle must be more than 0"));
        arcObject(f, center, QPointF(r, 0), QPointF(0, r), std::min(sector, 2 * M_PI), rot);
    }
    void ellipse(const Node& f)
    {
        const QPointF center = lengths(f.pair(QStringLiteral("center")));
        const QPointF axes = lengths(f.pair(QStringLiteral("semiaxes")));
        const double rot = angle(f.text(QStringLiteral("rotation")));
        if (!a_error.isEmpty()) return;
        if (!(axes.x() > 0) || !(axes.y() > 0)) return fail(tr("an ellipse's semiaxes must be more than 0"));
        const double c = std::cos(rot), s = std::sin(rot);
        arcObject(f, center, QPointF(axes.x() * c, axes.x() * s), QPointF(-axes.y() * s, axes.y() * c), 2 * M_PI, 0);
    }
    void polygon(const Node& f)
    {
        std::vector<QPointF> corners;
        for (const QJsonValue& v : f.value(QStringLiteral("points")).toArray()) {
            const QJsonArray p = v.toArray();
            auto txt = [](const QJsonValue& x) { return x.isDouble() ? formatNumber(x.toDouble(), 15) : x.toString(); };
            corners.push_back({length(txt(p.at(0))), length(txt(p.at(1)))});
        }
        if (!a_error.isEmpty()) return;
        const bool closed = f.flag(QStringLiteral("closed"));
        if (corners.size() < (closed ? 3u : 2u))
            return fail(closed ? tr("a polygon needs three corners at least") : tr("a polyline needs two points at least"));
        GObject o;
        if (closed) {
            std::vector<GEdge> loop = lineLoop(corners);
            if (std::abs(signedArea(loop)) == 0) return fail(tr("the polygon has no area"));
            if (signedArea(loop) < 0) reverse(loop);
            o.loops.push_back(loop);
        } else {
            for (std::size_t i = 0; i + 1 < corners.size(); ++i) {
                if (corners[i] == corners[i + 1]) continue;
                Curve c;
                c.c = corners[i];
                c.a = corners[i + 1] - corners[i];
                o.open.push_back({corners[i], corners[i + 1], addCurve(c), 0, 1});
            }
        }
        addObject(std::move(o), f);
    }
    void point(const Node& f)
    {
        const QPointF p = lengths(f.pair(QStringLiteral("position")));
        if (!a_error.isEmpty()) return;
        GObject o;
        o.points.push_back(p);
        addObject(std::move(o), f);
    }

    /// The objects named in \a key, taken out of the list (when \a take)
    /// or copied; an error when one is not there.
    std::vector<GObject> inputs(const Node& f, const QString& key, bool take)
    {
        std::vector<GObject> out;
        const QStringList names = f.list(key);
        if (names.isEmpty()) {
            fail(tr("no input objects"));
            return out;
        }
        for (const QString& n : names) {
            auto it = std::find_if(a_objects.begin(), a_objects.end(), [&](const GObject& o) { return o.name == n; });
            if (it == a_objects.end()) {
                fail(tr("there is no object %1 here (the objects: %2)").arg(n, objectList()));
                return {};
            }
            out.push_back(*it);
            if (take) a_objects.erase(it);
        }
        return out;
    }
    QString objectList() const
    {
        QStringList names;
        for (const GObject& o : a_objects) names << o.name;
        return names.isEmpty() ? tr("none") : names.join(QLatin1String(", "));
    }
    GObject mapped(const GObject& o, const Affine& m)
    {
        GObject out;
        out.name = o.name;
        QHash<int, int> curveMap;
        auto mapCurve = [&](int id) {
            auto it = curveMap.constFind(id);
            if (it != curveMap.cend()) return *it;
            Curve c = a_curves[std::size_t(id)];
            c.c = m.map(c.c);
            c.a = m.vec(c.a);
            c.b = m.vec(c.b);
            const int n = addCurve(c);
            curveMap.insert(id, n);
            return n;
        };
        auto mapEdge = [&](const GEdge& e) { return GEdge{m.map(e.a), m.map(e.b), mapCurve(e.curve), e.ta, e.tb}; };
        for (const auto& loop : o.loops) {
            std::vector<GEdge> l;
            for (const GEdge& e : loop) l.push_back(mapEdge(e));
            if (m.det() < 0) reverse(l);
            out.loops.push_back(l);
        }
        for (const GEdge& e : o.open) out.open.push_back(mapEdge(e));
        for (QPointF p : o.points) out.points.push_back(m.map(p));
        return out;
    }
    void transform(const Node& f)
    {
        const bool keep = f.flag(QStringLiteral("keep"));
        std::vector<Affine> maps;
        if (f.type == QLatin1String("move")) {
            const QPointF d = lengths(f.pair(QStringLiteral("displacement")));
            maps.push_back(Affine::translate(d.x(), d.y()));
        } else if (f.type == QLatin1String("rotate")) {
            const double a = angle(f.text(QStringLiteral("angle")));
            maps.push_back(Affine::rotate(a, lengths(f.pair(QStringLiteral("center")))));
        } else if (f.type == QLatin1String("scale")) {
            const QStringList factor = f.pair(QStringLiteral("factor"));
            const double fx = number(factor.value(0), 1), fy = number(factor.value(1), fx);
            const QPointF c = lengths(f.pair(QStringLiteral("center")));
            if (fx == 0 || fy == 0) return fail(tr("a scale factor of 0"));
            Affine a;
            a.m11 = fx;
            a.m22 = fy;
            a.dx = c.x() - fx * c.x();
            a.dy = c.y() - fy * c.y();
            maps.push_back(a);
        } else if (f.type == QLatin1String("mirror")) {
            const QPointF p = lengths(f.pair(QStringLiteral("point")));
            const QStringList nt = f.pair(QStringLiteral("normal"));
            QPointF n(number(nt.value(0)), number(nt.value(1)));
            const double len = norm(n);
            if (len == 0) return fail(tr("the axis's normal is zero"));
            n /= len;
            // x' = x - 2 n (n·(x - p))
            Affine a;
            a.m11 = 1 - 2 * n.x() * n.x();
            a.m12 = -2 * n.x() * n.y();
            a.m21 = -2 * n.x() * n.y();
            a.m22 = 1 - 2 * n.y() * n.y();
            const QPointF shift = 2 * dot(n, p) * n;
            a.dx = shift.x();
            a.dy = shift.y();
            maps.push_back(a);
        } else {   // array
            const QStringList count = f.pair(QStringLiteral("count"));
            const int nx = int(std::lround(number(count.value(0), 1))), ny = int(std::lround(number(count.value(1), 1)));
            const QPointF d = lengths(f.pair(QStringLiteral("displacement")));
            if (nx < 1 || ny < 1 || nx * ny > 10000) return fail(tr("an array of 1 to 10000 copies"));
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i) maps.push_back(Affine::translate(i * d.x(), j * d.y()));
        }
        if (!a_error.isEmpty()) return;
        const bool array = f.type == QLatin1String("array");
        std::vector<GObject> in = inputs(f, QStringLiteral("input"), !keep && !array);
        if (!a_error.isEmpty()) return;
        if (array) {
            // The first copy is the input itself.
            for (const GObject& o : in) {
                auto it = std::find_if(a_objects.begin(), a_objects.end(), [&](const GObject& x) { return x.name == o.name; });
                if (it != a_objects.end()) a_objects.erase(it);
            }
        }
        const QString base = f.label.isEmpty() ? f.tag : f.label;
        int index = 0;
        for (const GObject& o : in)
            for (const Affine& m : maps) {
                GObject out = mapped(o, m);
                // One object in, one out: its name the feature's; else the
                // feature's numbered.
                out.name = in.size() * maps.size() == 1 ? base : base + QLatin1Char('_') + QString::number(++index);
                a_objects.push_back(std::move(out));
            }
    }

    // --- Booleans, on Clipper ------------------------------------------
    using Path = ClipperLib::Path;
    using Paths = ClipperLib::Paths;
    double clipperScale() const
    {
        double extent = 1;
        for (const GObject& o : a_objects) {
            const QRectF b = o.bounds();
            extent = std::max({extent, std::abs(b.left()), std::abs(b.right()), std::abs(b.top()), std::abs(b.bottom())});
        }
        // About 1e12 steps across the largest coordinate.
        return std::pow(2.0, std::floor(std::log2(1e12 / extent)));
    }
    Paths paths(const std::vector<GObject>& objects, double scale) const
    {
        Paths out;
        for (const GObject& o : objects)
            for (const auto& loop : o.loops) {
                Path p;
                for (const GEdge& e : loop)
                    p.emplace_back(ClipperLib::cInt(std::llround(e.a.x() * scale)), ClipperLib::cInt(std::llround(e.a.y() * scale)));
                out.push_back(p);
            }
        return out;
    }
    /// \a result's loops, each straight piece given back the curve of the
    /// input edge it lies on.
    GObject fromPaths(const Paths& result, const std::vector<GObject>& from, double scale)
    {
        std::vector<GEdge> edges;
        for (const GObject& o : from)
            for (const auto& loop : o.loops)
                for (const GEdge& e : loop) edges.push_back(e);
        // A grid of the input edges, to find each piece's.
        QRectF box;
        for (const GEdge& e : edges) box |= QRectF(e.a, e.b).normalized().adjusted(-1e-12, -1e-12, 1e-12, 1e-12);
        const int n = std::clamp(int(std::sqrt(double(edges.size()))), 1, 256);
        const double cw = std::max(box.width() / n, 1e-300), ch = std::max(box.height() / n, 1e-300);
        std::vector<std::vector<int>> grid(std::size_t(n * n));
        auto cell = [&](double x, double y) {
            const int i = std::clamp(int((x - box.left()) / cw), 0, n - 1), j = std::clamp(int((y - box.top()) / ch), 0, n - 1);
            return std::pair<int, int>(i, j);
        };
        for (int k = 0; k < int(edges.size()); ++k) {
            const auto [i0, j0] = cell(std::min(edges[std::size_t(k)].a.x(), edges[std::size_t(k)].b.x()),
                                       std::min(edges[std::size_t(k)].a.y(), edges[std::size_t(k)].b.y()));
            const auto [i1, j1] = cell(std::max(edges[std::size_t(k)].a.x(), edges[std::size_t(k)].b.x()),
                                       std::max(edges[std::size_t(k)].a.y(), edges[std::size_t(k)].b.y()));
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) grid[std::size_t(j * n + i)].push_back(k);
        }
        const double tol = 4.0 / scale + 1e-9 * std::hypot(box.width(), box.height());
        GObject out;
        for (const Path& p : result) {
            if (p.size() < 3) continue;
            std::vector<GEdge> loop;
            for (std::size_t i = 0; i < p.size(); ++i) {
                const QPointF a(double(p[i].X) / scale, double(p[i].Y) / scale);
                const QPointF b(double(p[(i + 1) % p.size()].X) / scale, double(p[(i + 1) % p.size()].Y) / scale);
                if (a == b) continue;
                const QPointF mid = (a + b) / 2;
                const auto [ci, cj] = cell(mid.x(), mid.y());
                int found = -1;
                double best = tol;
                for (int k : grid[std::size_t(cj * n + ci)]) {
                    const GEdge& e = edges[std::size_t(k)];
                    const double d = std::max(segmentDistance(a, e.a, e.b), segmentDistance(b, e.a, e.b));
                    if (d <= best) {
                        best = d;
                        found = k;
                    }
                }
                GEdge piece{a, b, -1, 0, 1};
                if (found >= 0) {
                    const GEdge& e = edges[std::size_t(found)];
                    const Curve& c = a_curves[std::size_t(e.curve)];
                    piece.curve = e.curve;
                    piece.ta = c.paramOf(a);
                    piece.tb = c.paramOf(b);
                } else {
                    Curve c;
                    c.c = a;
                    c.a = b - a;
                    piece.curve = addCurve(c);
                }
                loop.push_back(piece);
            }
            if (loop.size() >= 3) out.loops.push_back(loop);
        }
        return out;
    }
    void boolean(const Node& f)
    {
        const bool keep = f.flag(QStringLiteral("keep"));
        const bool unite = f.type == QLatin1String("union");
        std::vector<GObject> in = inputs(f, QStringLiteral("input"), !keep || unite);
        if (!a_error.isEmpty()) return;
        std::vector<GObject> tools;
        if (f.type == QLatin1String("difference")) {
            tools = inputs(f, QStringLiteral("tools"), !keep);
            if (!a_error.isEmpty()) return;
        }
        for (const GObject& o : in)
            if (!o.solid()) return fail(tr("%1 is not a solid: only solids are united, cut or intersected").arg(o.name));
        GObject result;
        if (unite && f.flag(QStringLiteral("interior"))) {
            // Its interior boundaries kept: every input's loops, one object.
            for (const GObject& o : in)
                for (const auto& loop : o.loops) result.loops.push_back(loop);
        } else {
            const double scale = clipperScale();
            ClipperLib::Clipper clipper;
            Paths solution;
            if (unite) {
                clipper.AddPaths(paths(in, scale), ClipperLib::ptSubject, true);
                clipper.Execute(ClipperLib::ctUnion, solution, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
            } else if (f.type == QLatin1String("difference")) {
                clipper.AddPaths(paths(in, scale), ClipperLib::ptSubject, true);
                clipper.AddPaths(paths(tools, scale), ClipperLib::ptClip, true);
                clipper.Execute(ClipperLib::ctDifference, solution, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
            } else {
                // What every input has: one after another.
                Paths acc = paths({in.front()}, scale);
                for (std::size_t i = 1; i < in.size(); ++i) {
                    ClipperLib::Clipper c;
                    c.AddPaths(acc, ClipperLib::ptSubject, true);
                    c.AddPaths(paths({in[i]}, scale), ClipperLib::ptClip, true);
                    Paths next;
                    c.Execute(ClipperLib::ctIntersection, next, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
                    acc = next;
                }
                solution = acc;
            }
            ClipperLib::CleanPolygons(solution, 1.0);
            std::vector<GObject> all = in;
            all.insert(all.end(), tools.begin(), tools.end());
            result = fromPaths(solution, all, scale);
            if (!result.solid()) return fail(tr("nothing is left: the result is empty"));
        }
        addObject(std::move(result), f);
    }

    // --- Form Union ------------------------------------------------------
    std::shared_ptr<Topology> formUnion(double relTolerance, QString* why);

    const Model& a_model;
    const ParameterScope& a_ps;
    double a_scale = 1e-3;
    QString a_unitName;
    QString a_tag;
    QString a_error, a_warning;
    std::vector<Curve> a_curves;
    std::vector<GObject> a_objects;
};

struct RawSegment {
    int v0, v1;
    int curve;
    double t0, t1;
    QSet<int> objects;
};

/// Points within a tolerance made one, on a grid of cells that wide.
class VertexPool
{
public:
    explicit VertexPool(double eps) : a_eps(eps) {}
    int add(QPointF p)
    {
        const long long ix = std::llround(std::floor(p.x() / a_eps)), iy = std::llround(std::floor(p.y() / a_eps));
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy) {
                auto it = a_cells.find(key(ix + dx, iy + dy));
                if (it == a_cells.end()) continue;
                for (int v : it->second)
                    if (norm(vertices[std::size_t(v)] - p) <= a_eps) return v;
            }
        vertices.push_back(p);
        a_cells[key(ix, iy)].push_back(int(vertices.size()) - 1);
        return int(vertices.size()) - 1;
    }
    std::vector<QPointF> vertices;

private:
    static unsigned long long key(long long x, long long y)
    {
        return (static_cast<unsigned long long>(x) * 0x9E3779B97F4A7C15ull) ^ static_cast<unsigned long long>(y);
    }
    double a_eps;
    std::unordered_map<unsigned long long, std::vector<int>> a_cells;
};

std::shared_ptr<Topology> Builder::formUnion(double relTolerance, QString* why)
{
    auto topo = std::make_shared<Topology>();
    topo->unitScale = a_scale;
    topo->unitName = a_unitName;
    QRectF box;
    for (const GObject& o : a_objects) {
        topo->objectNames << o.name;
        const QRectF b = o.bounds();
        box = box.isNull() ? b : box.united(b);
    }
    topo->objects = a_objects;
    if (a_objects.empty() || box.isNull()) {
        // Nothing to make one: no domains.
        topo->curves = a_curves;
        topo->bounds = box;
        if (a_objects.empty()) *why = tr("the geometry has no objects");
        return topo;
    }
    const double diag = std::max(std::hypot(box.width(), box.height()), 1e-300);
    const double eps = relTolerance * diag;
    VertexPool pool(eps);
    std::vector<RawSegment> segs;
    for (int oi = 0; oi < int(a_objects.size()); ++oi) {
        const GObject& o = a_objects[std::size_t(oi)];
        auto addEdge = [&](const GEdge& e) {
            const int v0 = pool.add(e.a), v1 = pool.add(e.b);
            if (v0 == v1) return;
            segs.push_back({v0, v1, e.curve, e.ta, e.tb, {oi}});
        };
        for (const auto& loop : o.loops)
            for (const GEdge& e : loop) addEdge(e);
        for (const GEdge& e : o.open) addEdge(e);
        for (QPointF p : o.points) pool.add(p);
    }

    // Split where segments cross or touch, until none do.
    for (int pass = 0; pass < 6; ++pass) {
        const int n = int(segs.size());
        std::vector<std::vector<std::pair<double, int>>> splits(static_cast<std::size_t>(n));
        const int cells = std::clamp(int(std::sqrt(double(n))), 1, 512);
        const double cw = std::max(box.width() / cells, eps), ch = std::max(box.height() / cells, eps);
        std::vector<std::vector<int>> grid(std::size_t(cells * cells));
        auto cellOf = [&](double x, double y) {
            return std::pair<int, int>(std::clamp(int((x - box.left()) / cw), 0, cells - 1),
                                       std::clamp(int((y - box.top()) / ch), 0, cells - 1));
        };
        for (int k = 0; k < n; ++k) {
            const QPointF a = pool.vertices[std::size_t(segs[std::size_t(k)].v0)], b = pool.vertices[std::size_t(segs[std::size_t(k)].v1)];
            const auto [i0, j0] = cellOf(std::min(a.x(), b.x()) - eps, std::min(a.y(), b.y()) - eps);
            const auto [i1, j1] = cellOf(std::max(a.x(), b.x()) + eps, std::max(a.y(), b.y()) + eps);
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) grid[std::size_t(j * cells + i)].push_back(k);
        }
        std::unordered_map<unsigned long long, bool> tested;
        bool any = false;
        auto pointOn = [&](int k, int v) {
            // Vertex v on segment k, strictly between its ends.
            const RawSegment& s = segs[std::size_t(k)];
            if (v == s.v0 || v == s.v1) return;
            const QPointF a = pool.vertices[std::size_t(s.v0)], b = pool.vertices[std::size_t(s.v1)], p = pool.vertices[std::size_t(v)];
            double u = 0;
            const double d = segmentDistance(p, a, b, &u);
            const double len = norm(b - a);
            if (d <= eps && u * len > eps && (1 - u) * len > eps) {
                splits[std::size_t(k)].emplace_back(u, v);
                any = true;
            }
        };
        for (const auto& cellList : grid)
            for (std::size_t x = 0; x < cellList.size(); ++x)
                for (std::size_t y = x + 1; y < cellList.size(); ++y) {
                    const int i = std::min(cellList[x], cellList[y]), j = std::max(cellList[x], cellList[y]);
                    const unsigned long long key = (static_cast<unsigned long long>(i) << 32) | static_cast<unsigned long long>(j);
                    if (!tested.emplace(key, true).second) continue;
                    const RawSegment& s = segs[std::size_t(i)];
                    const RawSegment& r = segs[std::size_t(j)];
                    pointOn(i, r.v0);
                    pointOn(i, r.v1);
                    pointOn(j, s.v0);
                    pointOn(j, s.v1);
                    // A proper crossing.
                    const QPointF p0 = pool.vertices[std::size_t(s.v0)], p1 = pool.vertices[std::size_t(s.v1)];
                    const QPointF q0 = pool.vertices[std::size_t(r.v0)], q1 = pool.vertices[std::size_t(r.v1)];
                    const QPointF d1 = p1 - p0, d2 = q1 - q0;
                    const double den = cross(d1, d2);
                    if (std::abs(den) <= 1e-14 * norm(d1) * norm(d2)) continue;   // parallel
                    const double u = cross(q0 - p0, d2) / den, w = cross(q0 - p0, d1) / den;
                    const double l1 = norm(d1), l2 = norm(d2);
                    if (u * l1 <= eps || (1 - u) * l1 <= eps || w * l2 <= eps || (1 - w) * l2 <= eps) continue;
                    if (u <= 0 || u >= 1 || w <= 0 || w >= 1) continue;
                    const int v = pool.add(p0 + d1 * u);
                    if (v != s.v0 && v != s.v1) splits[std::size_t(i)].emplace_back(u, v);
                    if (v != r.v0 && v != r.v1) splits[std::size_t(j)].emplace_back(w, v);
                    any = true;
                }
        if (!any) break;
        std::vector<RawSegment> next;
        for (int k = 0; k < n; ++k) {
            const RawSegment& s = segs[std::size_t(k)];
            auto& sp = splits[std::size_t(k)];
            if (sp.empty()) {
                next.push_back(s);
                continue;
            }
            std::sort(sp.begin(), sp.end());
            int from = s.v0;
            double uFrom = 0;
            for (const auto& [u, v] : sp) {
                if (v == from) continue;
                next.push_back({from, v, s.curve, s.t0 + (s.t1 - s.t0) * uFrom, s.t0 + (s.t1 - s.t0) * u, s.objects});
                from = v;
                uFrom = u;
            }
            if (from != s.v1) next.push_back({from, s.v1, s.curve, s.t0 + (s.t1 - s.t0) * uFrom, s.t1, s.objects});
        }
        segs = std::move(next);
    }
    // One segment where several lie on each other: their objects together.
    {
        std::map<std::pair<int, int>, int> byEnds;
        std::vector<RawSegment> unique;
        for (const RawSegment& s : segs) {
            if (s.v0 == s.v1) continue;
            const std::pair<int, int> key(std::min(s.v0, s.v1), std::max(s.v0, s.v1));
            auto it = byEnds.find(key);
            if (it == byEnds.end()) {
                byEnds.emplace(key, int(unique.size()));
                unique.push_back(s);
            } else {
                unique[std::size_t(it->second)].objects.unite(s.objects);
            }
        }
        segs = std::move(unique);
    }

    // The planar graph triangulated: the triangles between its segments
    // are the domains'.
    CDT::Triangulation<double> cdt(CDT::VertexInsertionOrder::Auto, CDT::IntersectingConstraintEdges::TryResolve, eps);
    try {
        std::vector<CDT::V2d<double>> pts;
        pts.reserve(pool.vertices.size());
        for (QPointF p : pool.vertices) pts.emplace_back(p.x(), p.y());
        cdt.insertVertices(pts);
        std::vector<CDT::Edge> edges;
        edges.reserve(segs.size());
        for (const RawSegment& s : segs) edges.emplace_back(CDT::VertInd(s.v0), CDT::VertInd(s.v1));
        cdt.insertEdges(edges);
        cdt.eraseSuperTriangle();
    } catch (const std::exception& e) {
        *why = tr("Form Union failed: %1").arg(QString::fromLocal8Bit(e.what()));
        return nullptr;
    }
    // Vertices CDT added (where segments still crossed): ours too, the
    // segments' pieces given their originals' curves and objects.
    topo->vertices.clear();
    for (const auto& v : cdt.vertices) topo->vertices.emplace_back(v.x, v.y);
    std::map<std::pair<int, int>, int> segIndex;
    for (int k = 0; k < int(segs.size()); ++k)
        segIndex.emplace(std::pair<int, int>(std::min(segs[std::size_t(k)].v0, segs[std::size_t(k)].v1),
                                             std::max(segs[std::size_t(k)].v0, segs[std::size_t(k)].v1)), k);
    std::map<std::pair<int, int>, int> fixedIndex;   // a fixed edge's segment in the topology
    {
        std::vector<CDT::Edge> fixed(cdt.fixedEdges.begin(), cdt.fixedEdges.end());
        std::sort(fixed.begin(), fixed.end());
        for (const CDT::Edge& e : fixed) {
            const int a = int(e.v1()), b = int(e.v2());
            int original = -1;
            auto direct = segIndex.find({a, b});
            if (direct != segIndex.end()) original = direct->second;
            else {
                auto po = cdt.pieceToOriginals.find(e);
                if (po != cdt.pieceToOriginals.end() && !po->second.empty()) {
                    const CDT::Edge& oe = po->second.front();
                    auto it = segIndex.find({int(oe.v1()), int(oe.v2())});
                    if (it != segIndex.end()) original = it->second;
                }
            }
            Topology::Segment s;
            s.v0 = a;
            s.v1 = b;
            if (original >= 0) {
                const RawSegment& r = segs[std::size_t(original)];
                s.curve = r.curve;
                s.objects = r.objects;
                const Curve& c = a_curves[std::size_t(r.curve)];
                if (a == r.v0 && b == r.v1) {
                    s.t0 = r.t0;
                    s.t1 = r.t1;
                } else if (a == r.v1 && b == r.v0) {
                    s.t0 = r.t1;
                    s.t1 = r.t0;
                } else {
                    s.t0 = c.paramOf(topo->vertices[std::size_t(a)]);
                    s.t1 = c.paramOf(topo->vertices[std::size_t(b)]);
                }
            }
            fixedIndex.emplace(std::pair<int, int>(a, b), int(topo->segments.size()));
            topo->segments.push_back(s);
        }
    }
    auto segmentOf = [&](int a, int b) {
        auto it = fixedIndex.find({std::min(a, b), std::max(a, b)});
        return it == fixedIndex.end() ? -1 : it->second;
    };

    // The triangles in connected parts, none crossing a segment.
    const int nt = int(cdt.triangles.size());
    std::vector<int> part(std::size_t(nt), -1);
    std::vector<std::vector<int>> parts;
    for (int t = 0; t < nt; ++t) {
        if (part[std::size_t(t)] >= 0) continue;
        const int id = int(parts.size());
        parts.emplace_back();
        std::vector<int> stack{t};
        part[std::size_t(t)] = id;
        while (!stack.empty()) {
            const int cur = stack.back();
            stack.pop_back();
            parts.back().push_back(cur);
            const CDT::Triangle& tri = cdt.triangles[std::size_t(cur)];
            for (int k = 0; k < 3; ++k) {
                const CDT::TriInd nb = tri.neighbors[std::size_t(k)];
                if (nb == CDT::noNeighbor || part[nb] >= 0) continue;
                if (segmentOf(int(tri.vertices[std::size_t(k)]), int(tri.vertices[std::size_t((k + 1) % 3)])) >= 0) continue;
                part[nb] = id;
                stack.push_back(int(nb));
            }
        }
    }
    // Each object's area, for a domain's owner.
    std::vector<double> objectArea(a_objects.size(), 0);
    for (std::size_t oi = 0; oi < a_objects.size(); ++oi)
        for (const auto& loop : a_objects[oi].loops) objectArea[oi] += signedArea(loop);
    // Each part a domain when an object holds it.
    struct Found {
        Topology::Domain domain;
        std::vector<int> triangles;
    };
    std::vector<Found> found;
    std::vector<int> partDomain(parts.size(), -1);
    for (int p = 0; p < int(parts.size()); ++p) {
        double bestArea = -1;
        QPointF inside;
        for (int t : parts[std::size_t(p)]) {
            const CDT::Triangle& tri = cdt.triangles[std::size_t(t)];
            const QPointF a = topo->vertices[tri.vertices[0]], b = topo->vertices[tri.vertices[1]], c = topo->vertices[tri.vertices[2]];
            const double area = cross(b - a, c - a) / 2;
            if (area > bestArea) {
                bestArea = area;
                inside = (a + b + c) / 3;
            }
        }
        Topology::Domain d;
        double ownerArea = INFINITY;
        for (int oi = 0; oi < int(a_objects.size()); ++oi)
            if (a_objects[std::size_t(oi)].solid() && a_objects[std::size_t(oi)].contains(inside)) {
                d.objects.insert(oi);
                // Its owner: the smallest object that holds it (of two
                // alike, the later).
                const double a = objectArea[std::size_t(oi)];
                if (a <= ownerArea * (1 + 1e-12)) {
                    ownerArea = a;
                    d.owner = oi;
                }
            }
        if (d.objects.isEmpty()) continue;
        d.inside = inside;
        double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        for (int t : parts[std::size_t(p)]) {
            const CDT::Triangle& tri = cdt.triangles[std::size_t(t)];
            std::array<int, 3> v{int(tri.vertices[0]), int(tri.vertices[1]), int(tri.vertices[2])};
            d.triangles.push_back(v);
            const QPointF a = topo->vertices[std::size_t(v[0])], b = topo->vertices[std::size_t(v[1])], c = topo->vertices[std::size_t(v[2])];
            d.area += cross(b - a, c - a) / 2;
            for (QPointF q : {a, b, c}) {
                x0 = std::min(x0, q.x());
                y0 = std::min(y0, q.y());
                x1 = std::max(x1, q.x());
                y1 = std::max(y1, q.y());
            }
        }
        d.bounds = QRectF(QPointF(x0, y0), QPointF(x1, y1));
        partDomain[std::size_t(p)] = int(found.size());
        found.push_back({d, parts[std::size_t(p)]});
    }
    // Numbered as COMSOL numbers them: from the left, then from below.
    std::vector<int> order(found.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = int(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const QRectF& ra = found[std::size_t(a)].domain.bounds;
        const QRectF& rb = found[std::size_t(b)].domain.bounds;
        if (std::abs(ra.left() - rb.left()) > eps) return ra.left() < rb.left();
        if (std::abs(ra.top() - rb.top()) > eps) return ra.top() < rb.top();
        return found[std::size_t(a)].domain.inside.y() < found[std::size_t(b)].domain.inside.y();
    });
    std::vector<int> renumber(found.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        renumber[std::size_t(order[i])] = int(i);
        topo->domains.push_back(found[std::size_t(order[i])].domain);
    }
    // Each segment's domains on either side.
    for (int p = 0; p < int(parts.size()); ++p) {
        if (partDomain[std::size_t(p)] < 0) continue;
        const int d = renumber[std::size_t(partDomain[std::size_t(p)])];
        for (int t : parts[std::size_t(p)]) {
            const CDT::Triangle& tri = cdt.triangles[std::size_t(t)];
            for (int k = 0; k < 3; ++k) {
                const int a = int(tri.vertices[std::size_t(k)]), b = int(tri.vertices[std::size_t((k + 1) % 3)]);
                const int s = segmentOf(a, b);
                if (s < 0) continue;
                Topology::Segment& seg = topo->segments[std::size_t(s)];
                // The triangle is on the left of a -> b (counter-clockwise).
                if (seg.v0 == a) seg.left = d;
                else seg.right = d;
            }
        }
    }
    // The segments by a domain are kept; those outside every one go.
    std::vector<int> degree(topo->vertices.size(), 0);
    std::vector<bool> kept(topo->segments.size(), false);
    for (std::size_t s = 0; s < topo->segments.size(); ++s) {
        const Topology::Segment& seg = topo->segments[s];
        kept[s] = (seg.left >= 0 || seg.right >= 0) && seg.curve >= 0;
        if (kept[s]) {
            ++degree[std::size_t(seg.v0)];
            ++degree[std::size_t(seg.v1)];
        }
    }
    std::vector<std::vector<int>> around(topo->vertices.size());
    for (int s = 0; s < int(topo->segments.size()); ++s)
        if (kept[std::size_t(s)]) {
            around[std::size_t(topo->segments[std::size_t(s)].v0)].push_back(s);
            around[std::size_t(topo->segments[std::size_t(s)].v1)].push_back(s);
        }
    // A vertex ends a boundary where its segments are not alike: another
    // curve, other objects, other domains beside them.
    auto sides = [&](int s, int through, bool into) {
        const Topology::Segment& seg = topo->segments[std::size_t(s)];
        const bool forward = into ? seg.v1 == through : seg.v0 == through;
        return forward ? std::pair<int, int>(seg.left, seg.right) : std::pair<int, int>(seg.right, seg.left);
    };
    std::vector<bool> isBreak(topo->vertices.size(), false);
    for (int v = 0; v < int(topo->vertices.size()); ++v) {
        if (degree[std::size_t(v)] == 0) continue;
        if (degree[std::size_t(v)] != 2) {
            isBreak[std::size_t(v)] = true;
            continue;
        }
        const int s1 = around[std::size_t(v)][0], s2 = around[std::size_t(v)][1];
        const Topology::Segment& a = topo->segments[std::size_t(s1)];
        const Topology::Segment& b = topo->segments[std::size_t(s2)];
        if (a.curve != b.curve || a.objects != b.objects || sides(s1, v, true) != sides(s2, v, false)) isBreak[std::size_t(v)] = true;
    }
    std::vector<int> boundaryOf(topo->segments.size(), -1);
    std::vector<Topology::Boundary> chains;
    auto walk = [&](int startSeg) {
        // Back to where the chain begins, then forward along it.
        int seg = startSeg;
        int at = topo->segments[std::size_t(seg)].v0;
        for (std::size_t guard = 0; guard < topo->segments.size(); ++guard) {
            if (isBreak[std::size_t(at)]) break;
            const auto& ar = around[std::size_t(at)];
            const int prev = ar[0] == seg ? ar[1] : ar[0];
            if (prev == startSeg) {   // a closed loop with no point on it
                isBreak[std::size_t(at)] = true;
                break;
            }
            seg = prev;
            const Topology::Segment& ps = topo->segments[std::size_t(seg)];
            at = ps.v0 == at ? ps.v1 : ps.v0;
        }
        Topology::Boundary b;
        b.start = at;
        int cur = at;
        for (std::size_t guard = 0; guard <= topo->segments.size(); ++guard) {
            const Topology::Segment& s = topo->segments[std::size_t(seg)];
            const bool reversed = s.v1 == cur;
            b.segments.push_back(seg);
            b.reversed.push_back(reversed);
            boundaryOf[std::size_t(seg)] = int(chains.size());
            cur = reversed ? s.v0 : s.v1;
            if (isBreak[std::size_t(cur)]) break;
            const auto& ar = around[std::size_t(cur)];
            const int next = ar[0] == seg ? ar[1] : ar[0];
            if (boundaryOf[std::size_t(next)] >= 0) break;
            seg = next;
        }
        b.end = cur;
        chains.push_back(b);
    };
    for (int s = 0; s < int(topo->segments.size()); ++s)
        if (kept[std::size_t(s)] && boundaryOf[std::size_t(s)] < 0) walk(s);
    // Each boundary from its lower left end; its curve, its domains.
    for (Topology::Boundary& b : chains) {
        const QPointF ps = topo->vertices[std::size_t(b.start)], pe = topo->vertices[std::size_t(b.end)];
        if (b.start != b.end && (pe.x() < ps.x() - eps || (std::abs(pe.x() - ps.x()) <= eps && pe.y() < ps.y()))) {
            std::reverse(b.segments.begin(), b.segments.end());
            std::reverse(b.reversed.begin(), b.reversed.end());
            for (std::size_t i = 0; i < b.reversed.size(); ++i) b.reversed[i] = !b.reversed[i];
            std::swap(b.start, b.end);
        }
        const Topology::Segment& first = topo->segments[std::size_t(b.segments.front())];
        b.up = b.reversed.front() ? first.right : first.left;
        b.down = b.reversed.front() ? first.left : first.right;
        b.objects = first.objects;
        b.curve = first.curve;
        for (int s : b.segments)
            if (topo->segments[std::size_t(s)].curve != b.curve) b.curve = -1;
        if (b.curve >= 0) {
            const Topology::Segment& last = topo->segments[std::size_t(b.segments.back())];
            b.t0 = b.reversed.front() ? first.t1 : first.t0;
            b.t1 = b.reversed.back() ? last.t0 : last.t1;
        }
        for (int s : b.segments) {
            const Topology::Segment& seg = topo->segments[std::size_t(s)];
            b.length += norm(topo->vertices[std::size_t(seg.v1)] - topo->vertices[std::size_t(seg.v0)]);
        }
    }
    std::vector<int> border(chains.size());
    for (std::size_t i = 0; i < border.size(); ++i) border[i] = int(i);
    auto lowerLeft = [&](const Topology::Boundary& b) {
        double x = INFINITY, y = INFINITY;
        for (QPointF p : b.polyline(*topo)) {
            if (p.x() < x - eps || (std::abs(p.x() - x) <= eps && p.y() < y)) {
                x = p.x();
                y = p.y();
            }
        }
        return QPointF(x, y);
    };
    std::vector<QPointF> corner(chains.size());
    std::vector<QPointF> middle(chains.size());
    for (std::size_t i = 0; i < chains.size(); ++i) {
        corner[i] = lowerLeft(chains[i]);
        const std::vector<QPointF> pl = chains[i].polyline(*topo);
        middle[i] = pl[pl.size() / 2];
    }
    std::sort(border.begin(), border.end(), [&](int a, int b) {
        const QPointF ca = corner[std::size_t(a)], cb = corner[std::size_t(b)];
        if (std::abs(ca.x() - cb.x()) > eps) return ca.x() < cb.x();
        if (std::abs(ca.y() - cb.y()) > eps) return ca.y() < cb.y();
        const QPointF ma = middle[std::size_t(a)], mb = middle[std::size_t(b)];
        if (std::abs(ma.x() - mb.x()) > eps) return ma.x() < mb.x();
        return ma.y() < mb.y();
    });
    std::vector<int> boundaryNumber(chains.size());
    for (std::size_t i = 0; i < border.size(); ++i) {
        boundaryNumber[std::size_t(border[i])] = int(i);
        topo->boundaries.push_back(chains[std::size_t(border[i])]);
    }
    for (std::size_t s = 0; s < topo->segments.size(); ++s)
        topo->segments[s].boundary = boundaryOf[s] >= 0 ? boundaryNumber[std::size_t(boundaryOf[s])] : -1;
    // The points: where boundaries end, and points of the geometry inside
    // a domain.
    std::vector<int> pointVertices;
    for (const Topology::Boundary& b : topo->boundaries) {
        pointVertices.push_back(b.start);
        pointVertices.push_back(b.end);
    }
    for (const GObject& o : a_objects)
        for (QPointF p : o.points) {
            // (the vertex the pool made of it)
            int best = -1;
            double bestDist = eps * 2;
            for (int v = 0; v < int(topo->vertices.size()); ++v) {
                const double d = norm(topo->vertices[std::size_t(v)] - p);
                if (d <= bestDist) {
                    bestDist = d;
                    best = v;
                }
            }
            if (best >= 0 && (degree[std::size_t(best)] > 0 || topo->domainAt(p) >= 0)) pointVertices.push_back(best);
        }
    std::sort(pointVertices.begin(), pointVertices.end());
    pointVertices.erase(std::unique(pointVertices.begin(), pointVertices.end()), pointVertices.end());
    std::sort(pointVertices.begin(), pointVertices.end(), [&](int a, int b) {
        const QPointF pa = topo->vertices[std::size_t(a)], pb = topo->vertices[std::size_t(b)];
        if (std::abs(pa.x() - pb.x()) > eps) return pa.x() < pb.x();
        return pa.y() < pb.y();
    });
    QHash<int, int> pointOfVertex;
    for (int v : pointVertices) {
        Topology::Point p;
        p.vertex = v;
        pointOfVertex.insert(v, int(topo->points.size()));
        topo->points.push_back(p);
    }
    for (int b = 0; b < int(topo->boundaries.size()); ++b) {
        const Topology::Boundary& bd = topo->boundaries[std::size_t(b)];
        for (int v : {bd.start, bd.end}) {
            Topology::Point& p = topo->points[std::size_t(pointOfVertex.value(v))];
            if (std::find(p.boundaries.begin(), p.boundaries.end(), b) == p.boundaries.end()) p.boundaries.push_back(b);
            p.objects.unite(bd.objects);
        }
    }
    for (int oi = 0; oi < int(a_objects.size()); ++oi)
        for (QPointF q : a_objects[std::size_t(oi)].points) {
            const int pi = topo->pointAt(q, eps * 2);
            if (pi >= 0) topo->points[std::size_t(pi)].objects.insert(oi);
        }
    topo->curves = a_curves;
    topo->bounds = box;
    if (topo->domains.empty()) *why = tr("the geometry has no domains: no solid object");
    return topo;
}

} // namespace

GeometryBuild buildGeometry(const Model& model, const ParameterScope& parameters, const QString& upTo)
{
    Builder builder(model, parameters);
    return builder.build(upTo);
}

// ------------------------------------------------------------------ Selections

namespace {

QSet<int> objectIndices(const Topology& t, const QJsonValue& names, QStringList& unknown)
{
    QSet<int> out;
    QStringList list;
    if (names.isString()) list << names.toString();
    for (const QJsonValue& v : names.toArray()) list << v.toString();
    for (const QString& n : list) {
        bool any = false;
        for (int i = 0; i < t.objectNames.size(); ++i)
            if (t.objectNames.at(i) == n) {
                out.insert(i);
                any = true;
            }
        if (!any) unknown << n;
    }
    return out;
}

int count(const Topology& t, Level level)
{
    switch (level) {
    case Level::Domain: return int(t.domains.size());
    case Level::Boundary: return int(t.boundaries.size());
    case Level::Point: return int(t.points.size());
    case Level::None: break;
    }
    return 0;
}

bool insideBox(const Topology& t, Level level, int i, const QRectF& box)
{
    auto in = [&](QPointF p) { return box.contains(p); };
    if (level == Level::Domain) return box.contains(t.domains[std::size_t(i)].bounds);
    if (level == Level::Boundary) {
        for (QPointF p : t.boundaries[std::size_t(i)].polyline(t))
            if (!in(p)) return false;
        return true;
    }
    return in(t.vertices[std::size_t(t.points[std::size_t(i)].vertex)]);
}

QSet<int> resolve(const Topology& t, const Model& model, Level level, const QJsonObject& rule, QStringList& problems, int depth)
{
    QSet<int> out;
    if (depth > 16) {
        problems << tr("the selections name each other in a circle");
        return out;
    }
    const int n = count(t, level);
    QStringList unknown;
    if (rule.value(QStringLiteral("all")).toBool()) {
        for (int i = 0; i < n; ++i) out.insert(i);
    } else if (rule.contains(QStringLiteral("numbers"))) {
        for (const QJsonValue& v : rule.value(QStringLiteral("numbers")).toArray()) {
            const int k = v.toInt() - 1;
            if (k >= 0 && k < n) out.insert(k);
            else problems << tr("there is no %1 %2").arg(levelName(level)).arg(v.toInt());
        }
    } else if (rule.contains(QStringLiteral("named"))) {
        const QString name = rule.value(QStringLiteral("named")).toString();
        const Node* found = nullptr;
        if (const Node* sels = model.component().child(QStringLiteral("selections")))
            for (const Node& s : sels->children)
                if ((s.label == name || s.tag == name) && s.enabled) found = &s;
        if (!found) problems << tr("there is no selection %1").arg(name);
        else if (levelOf(found->text(QStringLiteral("level"))) != level)
            problems << tr("the selection %1 is of %2s, not %3s").arg(name, found->text(QStringLiteral("level")), levelName(level));
        else out = resolve(t, model, level, found->selection(), problems, depth + 1);
    } else if (rule.contains(QStringLiteral("box"))) {
        const QJsonArray b = rule.value(QStringLiteral("box")).toArray();
        auto num = [](const QJsonValue& v) { return v.isDouble() ? v.toDouble() : v.toString().toDouble(); };
        const QRectF box = QRectF(QPointF(num(b.at(0)), num(b.at(1))), QPointF(num(b.at(2)), num(b.at(3)))).normalized();
        const double tol = 1e-9 * std::max(1.0, std::hypot(t.bounds.width(), t.bounds.height()));
        const QRectF grown = box.adjusted(-tol, -tol, tol, tol);
        for (int i = 0; i < n; ++i)
            if (insideBox(t, level, i, grown)) out.insert(i);
    } else if (level == Level::Domain && (rule.contains(QStringLiteral("objects")) || rule.contains(QStringLiteral("only")))) {
        const bool only = rule.contains(QStringLiteral("only"));
        const QSet<int> objs = objectIndices(t, rule.value(only ? QStringLiteral("only") : QStringLiteral("objects")), unknown);
        for (int i = 0; i < n; ++i) {
            const Topology::Domain& d = t.domains[std::size_t(i)];
            if (only ? objs.contains(d.owner) : d.objects.intersects(objs)) out.insert(i);
        }
    } else if (rule.contains(QStringLiteral("of")) && level != Level::Domain) {
        const QSet<int> objs = objectIndices(t, rule.value(QStringLiteral("of")), unknown);
        const QString side = rule.value(QStringLiteral("side")).toString();
        QPointF dir;
        if (side == QLatin1String("bottom")) dir = {0, -1};
        else if (side == QLatin1String("top")) dir = {0, 1};
        else if (side == QLatin1String("left")) dir = {-1, 0};
        else if (side == QLatin1String("right")) dir = {1, 0};
        else if (!side.isEmpty()) problems << tr("a side is bottom, top, left or right, not %1").arg(side);
        QSet<int> boundaries;
        for (int b = 0; b < int(t.boundaries.size()); ++b) {
            const Topology::Boundary& bd = t.boundaries[std::size_t(b)];
            if (!bd.objects.intersects(objs)) continue;
            if (!dir.isNull()) {
                // Facing the side: the normal out of the objects' domain.
                auto held = [&](int d) { return d >= 0 && t.domains[std::size_t(d)].objects.intersects(objs); };
                const bool upIn = held(bd.up), downIn = held(bd.down);
                if (upIn == downIn) continue;
                const QPointF nrm = t.normalAt(b, upIn ? bd.up : bd.down);
                if (dot(nrm, dir) < 0.7) continue;
            }
            boundaries.insert(b);
        }
        if (level == Level::Boundary) out = boundaries;
        else
            for (int b : boundaries)
                for (int p = 0; p < int(t.points.size()); ++p)
                    if (std::find(t.points[std::size_t(p)].boundaries.begin(), t.points[std::size_t(p)].boundaries.end(), b)
                        != t.points[std::size_t(p)].boundaries.end())
                        out.insert(p);
        if (level == Level::Point && dir.isNull())
            for (int p = 0; p < int(t.points.size()); ++p)
                if (t.points[std::size_t(p)].objects.intersects(objs)) out.insert(p);
    } else if (level == Level::Boundary && rule.contains(QStringLiteral("between"))) {
        const QJsonArray pair = rule.value(QStringLiteral("between")).toArray();
        const QSet<int> a = objectIndices(t, pair.at(0), unknown), b = objectIndices(t, pair.at(1), unknown);
        auto owner = [&](int d) { return d >= 0 ? t.domains[std::size_t(d)].owner : -1; };
        for (int i = 0; i < n; ++i) {
            const Topology::Boundary& bd = t.boundaries[std::size_t(i)];
            const int u = owner(bd.up), d = owner(bd.down);
            if ((a.contains(u) && b.contains(d)) || (a.contains(d) && b.contains(u))) out.insert(i);
        }
    } else if (level == Level::Boundary && (rule.value(QStringLiteral("exterior")).toBool() || rule.value(QStringLiteral("interior")).toBool())) {
        const bool exterior = rule.value(QStringLiteral("exterior")).toBool();
        for (int i = 0; i < n; ++i) {
            const Topology::Boundary& bd = t.boundaries[std::size_t(i)];
            const bool ext = bd.exterior();
            const bool inner = bd.up >= 0 && bd.down >= 0;
            if (exterior ? ext : inner) out.insert(i);
        }
    } else if (rule.contains(QStringLiteral("adjacent")) && level != Level::Domain) {
        const QSet<int> domains = resolve(t, model, Level::Domain, rule.value(QStringLiteral("adjacent")).toObject(), problems, depth + 1);
        QSet<int> boundaries;
        for (int i = 0; i < int(t.boundaries.size()); ++i) {
            const Topology::Boundary& bd = t.boundaries[std::size_t(i)];
            if (domains.contains(bd.up) || domains.contains(bd.down)) boundaries.insert(i);
        }
        if (level == Level::Boundary) out = boundaries;
        else
            for (int p = 0; p < int(t.points.size()); ++p)
                for (int b : t.points[std::size_t(p)].boundaries)
                    if (boundaries.contains(b)) out.insert(p);
    } else if (!rule.isEmpty()) {
        problems << tr("a rule for %1s is all, numbers, %2box or named")
                        .arg(levelName(level),
                             level == Level::Domain ? QStringLiteral("objects, only, ")
                                                    : QStringLiteral("of (with a side), between, exterior, interior, adjacent, "));
    }
    if (!unknown.isEmpty()) problems << tr("there is no object %1").arg(unknown.join(QLatin1String(", ")));
    if (rule.contains(QStringLiteral("except"))) out.subtract(resolve(t, model, level, rule.value(QStringLiteral("except")).toObject(), problems, depth + 1));
    return out;
}

QString names(const QJsonValue& v)
{
    QStringList list;
    if (v.isString()) list << v.toString();
    for (const QJsonValue& e : v.toArray()) list << e.toString();
    return list.join(QLatin1String(", "));
}

} // namespace

QVector<int> resolveSelection(const Topology& topology, const Model& model, Level level, const QJsonObject& rule, QString* error)
{
    QStringList problems;
    const QSet<int> set = resolve(topology, model, level, rule, problems, 0);
    QVector<int> out(set.begin(), set.end());
    std::sort(out.begin(), out.end());
    if (error) *error = problems.join(QLatin1String("; "));
    return out;
}

QString describeSelection(const QJsonObject& rule, Level level)
{
    const QString plural = level == Level::Domain ? tr("domains") : level == Level::Boundary ? tr("boundaries") : tr("points");
    QString text;
    if (rule.value(QStringLiteral("all")).toBool()) text = tr("all %1").arg(plural);
    else if (rule.contains(QStringLiteral("numbers"))) {
        QStringList n;
        for (const QJsonValue& v : rule.value(QStringLiteral("numbers")).toArray()) n << QString::number(v.toInt());
        text = n.isEmpty() ? tr("no %1").arg(plural) : tr("%1 %2").arg(plural, n.join(QLatin1String(", ")));
    } else if (rule.contains(QStringLiteral("named"))) text = tr("the selection %1").arg(rule.value(QStringLiteral("named")).toString());
    else if (rule.contains(QStringLiteral("objects"))) text = tr("the %1 in %2").arg(plural, names(rule.value(QStringLiteral("objects"))));
    else if (rule.contains(QStringLiteral("only"))) text = tr("the %1 of %2 alone").arg(plural, names(rule.value(QStringLiteral("only"))));
    else if (rule.contains(QStringLiteral("of"))) {
        text = tr("the %1 of %2").arg(plural, names(rule.value(QStringLiteral("of"))));
        if (rule.contains(QStringLiteral("side"))) text += tr(" (%1)").arg(rule.value(QStringLiteral("side")).toString());
    } else if (rule.contains(QStringLiteral("between"))) {
        const QJsonArray p = rule.value(QStringLiteral("between")).toArray();
        text = tr("the %1 between %2 and %3").arg(plural, names(p.at(0)), names(p.at(1)));
    } else if (rule.value(QStringLiteral("exterior")).toBool()) text = tr("the exterior %1").arg(plural);
    else if (rule.value(QStringLiteral("interior")).toBool()) text = tr("the interior %1").arg(plural);
    else if (rule.contains(QStringLiteral("adjacent")))
        text = tr("the %1 of %2").arg(plural, describeSelection(rule.value(QStringLiteral("adjacent")).toObject(), Level::Domain));
    else if (rule.contains(QStringLiteral("box"))) text = tr("the %1 in a box").arg(plural);
    else text = tr("no %1").arg(plural);
    if (rule.contains(QStringLiteral("except")))
        text += tr(", except %1").arg(describeSelection(rule.value(QStringLiteral("except")).toObject(), level));
    return text;
}

} // namespace qucs_s::fem
