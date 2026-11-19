/*
 * multiphysicsstrokes.cpp - the pen's strokes on a 2D multiphysics model
 *                           read against its geometry, for Claude
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "multiphysicsstrokes.h"

#include "multiphysics/fem_geometry.h"

#include <QJsonObject>
#include <QLineF>
#include <QMap>

#include <algorithm>
#include <cmath>
#include <limits>

namespace qucs_s::fem {

namespace {

namespace markup = qucs_s::markup;

/// The most of each kind a reading names; the rest counted.
constexpr int MostNamed = 12;

QString number(double v)
{
    if (std::abs(v) < 1e-12) v = 0;
    return QString::number(v, 'g', 5);
}

QString pointText(QPointF p)
{
    return QStringLiteral("(%1, %2)").arg(number(p.x()), number(p.y()));
}

QJsonArray pointJson(QPointF p)
{
    const auto r = [](double v) { return std::abs(v) < 1e-12 ? 0.0 : QString::number(v, 'g', 6).toDouble(); };
    return QJsonArray{r(p.x()), r(p.y())};
}

QString domainName(const Topology& t, int d)
{
    const QString owner = t.objectNames.value(t.domains[std::size_t(d)].owner);
    return owner.isEmpty() ? QStringLiteral("domain %1").arg(d + 1) : QStringLiteral("domain %1 (%2)").arg(d + 1).arg(owner);
}

QString boundaryName(const Topology& t, int b)
{
    const Topology::Boundary& bd = t.boundaries[std::size_t(b)];
    const int up = bd.up, down = bd.down;
    if (up >= 0 && down >= 0) return QStringLiteral("boundary %1 (between domains %2 and %3)").arg(b + 1).arg(std::min(up, down) + 1).arg(std::max(up, down) + 1);
    if (up >= 0 || down >= 0) return QStringLiteral("boundary %1 (outer, of domain %2)").arg(b + 1).arg(std::max(up, down) + 1);
    return QStringLiteral("boundary %1").arg(b + 1);
}

QString pointName(const Topology& t, int i)
{
    const int v = t.points[std::size_t(i)].vertex;
    if (v < 0 || v >= int(t.vertices.size())) return QStringLiteral("point %1").arg(i + 1);
    return QStringLiteral("point %1 at %2").arg(i + 1).arg(pointText(t.vertices[std::size_t(v)]));
}

/// What \a at is on: the point within \a reach, else the boundary, else the
/// domain; empty outside the geometry.
QString entityAt(const Topology& t, QPointF at, double reach)
{
    if (const int p = t.pointAt(at, reach); p >= 0) return pointName(t, p);
    if (const int b = t.boundaryAt(at, reach); b >= 0) return boundaryName(t, b);
    if (const int d = t.domainAt(at); d >= 0) return domainName(t, d);
    return {};
}

/// \a names, the first MostNamed of them, the rest counted as \a kind.
QJsonArray named(const QStringList& names, const QString& kind)
{
    QJsonArray out;
    for (int i = 0; i < std::min<int>(names.size(), MostNamed); ++i) out.append(names.at(i));
    if (names.size() > MostNamed) out.append(QStringLiteral("%1 more %2").arg(names.size() - MostNamed).arg(kind));
    return out;
}

QPolygonF closed(const QPolygonF& points)
{
    QPolygonF ring = points;
    if (!ring.isEmpty()) ring << ring.first();
    return ring;
}

/// The boundaries a stroke's middle line lies along, those it follows for
/// a quarter of its length at least, nearest first - when most of it (seven
/// tenths) lies along one.
QStringList followed(const Topology& t, const markup::Stroke& s, double reach)
{
    const QPolygonF along = markup::alongOf(s);
    if (along.isEmpty()) return {};
    QMap<int, int> near;
    int onAny = 0;
    for (const QPointF& p : along)
        if (const int b = t.boundaryAt(p, reach); b >= 0) {
            ++near[b];
            ++onAny;
        }
    if (onAny < 0.7 * along.size()) return {};
    std::vector<std::pair<int, int>> ranked;   // (count, boundary)
    for (auto it = near.cbegin(); it != near.cend(); ++it)
        if (it.value() >= 0.25 * along.size()) ranked.emplace_back(it.value(), it.key());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
    QStringList out;
    for (const auto& [count, b] : ranked) out << boundaryName(t, b);
    return out;
}

/// The domains a stroke's middle line lies on, those under most of it first.
QStringList under(const Topology& t, const markup::Stroke& s)
{
    QMap<int, int> on;
    for (const QPointF& p : markup::alongOf(s))
        if (const int d = t.domainAt(p); d >= 0) ++on[d];
    std::vector<std::pair<int, int>> ranked;
    for (auto it = on.cbegin(); it != on.cend(); ++it) ranked.emplace_back(it.value(), it.key());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
    QStringList out;
    for (const auto& [count, d] : ranked) out << domainName(t, d);
    return out;
}

/// What a ring holds: the domains at least half inside it (by their
/// triangles' areas), those partly (a twentieth or more), the boundaries
/// four fifths inside, the points inside.
void heldBy(const Topology& t, const QPolygonF& ring, QJsonObject& o)
{
    QStringList domains, partly, boundaries, points;
    const QRectF box = ring.boundingRect();
    for (std::size_t d = 0; d < t.domains.size(); ++d) {
        const Topology::Domain& dom = t.domains[d];
        if (!dom.bounds.intersects(box) || dom.area <= 0) continue;
        double in = 0, all = 0;
        for (const std::array<int, 3>& tri : dom.triangles) {
            const QPointF a = t.vertices[std::size_t(tri[0])], b = t.vertices[std::size_t(tri[1])], c = t.vertices[std::size_t(tri[2])];
            const double area = std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2;
            all += area;
            if (ring.containsPoint((a + b + c) / 3, Qt::OddEvenFill)) in += area;
        }
        if (all <= 0) continue;
        if (in >= 0.5 * all) domains << domainName(t, int(d));
        else if (in >= 0.05 * all) partly << domainName(t, int(d));
    }
    for (std::size_t b = 0; b < t.boundaries.size(); ++b) {
        const std::vector<QPointF> line = t.boundaries[b].polyline(t);
        if (line.size() < 2) continue;
        // Its points and those between them, so that a straight boundary
        // of two ends is judged along its length.
        int in = 0, all = 0;
        for (std::size_t i = 0; i + 1 < line.size(); ++i)
            for (int k = 0; k < 8; ++k) {
                ++all;
                if (ring.containsPoint(line[i] + (line[i + 1] - line[i]) * (k / 8.0), Qt::OddEvenFill)) ++in;
            }
        ++all;
        if (ring.containsPoint(line.back(), Qt::OddEvenFill)) ++in;
        if (in >= 0.8 * all) boundaries << boundaryName(t, int(b));
    }
    for (std::size_t i = 0; i < t.points.size(); ++i) {
        const int v = t.points[i].vertex;
        if (v >= 0 && v < int(t.vertices.size()) && ring.containsPoint(t.vertices[std::size_t(v)], Qt::OddEvenFill)) points << pointName(t, int(i));
    }
    QJsonObject around;
    if (!domains.isEmpty()) around.insert(QStringLiteral("domains"), named(domains, QStringLiteral("domains")));
    if (!boundaries.isEmpty()) around.insert(QStringLiteral("boundaries"), named(boundaries, QStringLiteral("boundaries")));
    if (!points.isEmpty()) around.insert(QStringLiteral("points"), named(points, QStringLiteral("points")));
    if (!around.isEmpty()) o.insert(QStringLiteral("around"), around);
    if (!partly.isEmpty()) o.insert(QStringLiteral("around part of"), named(partly, QStringLiteral("domains")));
}

QString valueText(const ShownValues& values, QPointF at)
{
    if (!values) return {};
    const std::optional<double> v = values.at(at);
    if (!v || !std::isfinite(*v)) return {};
    return QStringLiteral("%1 = %2").arg(values.expression, values.said ? values.said(*v) : number(*v));
}

/// The least and greatest of the values shown inside \a ring: a grid of
/// 24 by 24 over its box, the points inside it.
void extremesIn(const ShownValues& values, const QPolygonF& ring, QJsonObject& o)
{
    const QRectF box = ring.boundingRect();
    if (box.isEmpty()) return;
    constexpr int n = 24;
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    QPointF loAt, hiAt;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const QPointF p(box.left() + box.width() * (i + 0.5) / n, box.top() + box.height() * (j + 0.5) / n);
            if (!ring.containsPoint(p, Qt::OddEvenFill)) continue;
            const std::optional<double> v = values.at(p);
            if (!v || !std::isfinite(*v)) continue;
            if (*v < lo) {
                lo = *v;
                loAt = p;
            }
            if (*v > hi) {
                hi = *v;
                hiAt = p;
            }
        }
    if (!std::isfinite(lo)) return;
    const auto said = [&values](double v) { return values.said ? values.said(v) : number(v); };
    o.insert(QStringLiteral("least inside"), QStringLiteral("%1 = %2 at %3").arg(values.expression, said(lo), pointText(loAt)));
    o.insert(QStringLiteral("greatest inside"), QStringLiteral("%1 = %2 at %3").arg(values.expression, said(hi), pointText(hiAt)));
}

QString listSaid(const QJsonValue& v)
{
    QStringList parts;
    if (v.isArray()) {
        for (const QJsonValue& one : v.toArray()) parts << one.toString();
    } else if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (const char* key : {"domains", "boundaries", "points"})
            for (const QJsonValue& one : o.value(QLatin1String(key)).toArray()) parts << one.toString();
    } else {
        parts << v.toString();
    }
    return parts.join(QStringLiteral(", "));
}

} // namespace

QJsonArray readModelStrokes(const Topology* topology, const QList<markup::Stroke>& strokes, bool points, const ShownValues& values)
{
    QJsonArray out;
    QList<QPointF> tips;
    QList<int> heads;
    const QList<markup::Gesture> gestures = markup::gesturesOf(strokes, &tips, &heads);
    for (int i = 0; i < strokes.size(); ++i) {
        const markup::Stroke& s = strokes.at(i);
        const markup::Gesture g = gestures.at(i);
        QJsonObject o = markup::readingOf(s, i + 1, g, points);
        if (s.points.isEmpty()) {
            out.append(o);
            continue;
        }
        const double reach = markup::reachOf(s);
        const auto put = [&o](const char* key, const QString& text) {
            if (!text.isEmpty()) o.insert(QLatin1String(key), text);
        };
        const auto on = [&](QPointF at) { return topology ? entityAt(*topology, at, reach) : QString(); };
        switch (g) {
        case markup::Gesture::Arrowhead: {
            const markup::Stroke& shaft = strokes.at(heads.at(i));
            o.insert(QStringLiteral("of stroke"), shaft.id > 0 ? shaft.id : heads.at(i) + 1);
            break;
        }
        case markup::Gesture::Dot:
            put("on", on(s.points.first()));
            put("value at", valueText(values, s.points.first()));
            break;
        case markup::Gesture::Corner:
            put("at", on(tips.at(i)));
            o.insert(QStringLiteral("point"), pointJson(tips.at(i)));
            put("value at", valueText(values, tips.at(i)));
            break;
        case markup::Gesture::Ring: {
            const QPolygonF ring = closed(s.points);
            if (topology) heldBy(*topology, ring, o);
            if (values) extremesIn(values, ring, o);
            break;
        }
        case markup::Gesture::Mark:
        case markup::Gesture::Line:
        case markup::Gesture::Arrow: {
            if (s.highlighter) {
                // What it lies along, else on.
                if (topology) {
                    QStringList lit = followed(*topology, s, reach);
                    if (lit.isEmpty()) lit = under(*topology, s);
                    if (!lit.isEmpty()) o.insert(QStringLiteral("highlights"), named(lit, QStringLiteral("more")));
                }
            } else if (g == markup::Gesture::Mark) {
                // A mark: the one thing it is on, at its middle.
                put("on", on(s.bounds().center()));
                put("value at", valueText(values, s.bounds().center()));
                break;
            }
            const QPointF start = s.points.first(), end = tips.at(i);
            if (topology && !s.highlighter) {
                const QStringList along = followed(*topology, s, reach);
                if (!along.isEmpty()) o.insert(QStringLiteral("along"), named(along, QStringLiteral("boundaries")));
            }
            put("from", on(start));
            put("points at", on(end));
            if (g == markup::Gesture::Arrow) {
                o.insert(QStringLiteral("tip"), pointJson(end));
                o.insert(QStringLiteral("tail"), pointJson(start));
                put("value at", valueText(values, end));
            } else if (g == markup::Gesture::Line) {
                put("value at its start", valueText(values, start));
                put("value at its end", valueText(values, end));
            } else {
                put("value at", valueText(values, s.bounds().center()));
            }
            break;
        }
        }
        out.append(o);
    }
    return out;
}

QString modelSentences(const QJsonArray& readings, const QString& unit, int most)
{
    QStringList lines;
    QJsonArray open;
    QStringList addressed;
    for (const QJsonValue& v : readings) {
        const QJsonObject r = v.toObject();
        if (r.value(QStringLiteral("addressed")).toBool()) addressed << QStringLiteral("stroke %1").arg(r.value(QStringLiteral("stroke")).toInt());
        else open.append(r);
    }
    const int from = std::max(0, int(open.size()) - most);
    if (from > 0) lines << QStringLiteral("(%1 earlier stroke(s) left out: read_drawings has them all)").arg(from);
    for (int i = from; i < open.size(); ++i) {
        const QJsonObject r = open.at(i).toObject();
        // Its start as every viewer says it (how drawn, tool, colour), then
        // what it marks here, then its box and its points.
        QJsonObject base;
        for (const char* key : {"stroke", "drawn as", "tool", "colour", "of stroke"})
            if (r.contains(QLatin1String(key))) base.insert(QLatin1String(key), r.value(QLatin1String(key)));
        QString s = markup::sentenceOf(base, QStringLiteral("the model"), unit);
        if (r.value(QStringLiteral("drawn as")).toString() != QLatin1String("arrowhead")) {
            const std::pair<const char*, const char*> fields[] = {
                {"around", "around"}, {"around part of", "around part of"}, {"highlights", "over"}, {"on", "on"},
                {"at", "its point on"}, {"along", "along"},      {"from", "from"},            {"points at", "pointing at"}};
            for (const auto& [key, words] : fields)
                if (r.contains(QLatin1String(key))) s += QStringLiteral(" %1 %2").arg(QLatin1String(words), listSaid(r.value(QLatin1String(key))));
            if (r.contains(QStringLiteral("tip"))) {
                const QJsonArray tip = r.value(QStringLiteral("tip")).toArray(), tail = r.value(QStringLiteral("tail")).toArray();
                s += QStringLiteral(", its tip at (%1, %2), its tail at (%3, %4)")
                         .arg(tip.at(0).toDouble()).arg(tip.at(1).toDouble()).arg(tail.at(0).toDouble()).arg(tail.at(1).toDouble());
            }
            if (r.contains(QStringLiteral("point"))) {
                const QJsonArray at = r.value(QStringLiteral("point")).toArray();
                s += QStringLiteral(", its point at (%1, %2)").arg(at.at(0).toDouble()).arg(at.at(1).toDouble());
            }
            for (const char* key : {"value at", "value at its start", "value at its end", "least inside", "greatest inside"})
                if (r.contains(QLatin1String(key))) s += QStringLiteral("; %1 %2").arg(QLatin1String(key), r.value(QLatin1String(key)).toString());
            const QJsonArray b = r.value(QStringLiteral("box")).toArray();
            if (b.size() == 4)
                s += QStringLiteral(" (its box %1, %2 to %3, %4 %5)")
                         .arg(b.at(0).toDouble()).arg(b.at(1).toDouble()).arg(b.at(2).toDouble()).arg(b.at(3).toDouble()).arg(unit);
            const QJsonArray pts = r.value(QStringLiteral("points")).toArray();
            if (!pts.isEmpty() && pts.size() <= markup::MostPointsSaid) {
                QStringList said;
                for (const QJsonValue& p : pts) said << QStringLiteral("(%1, %2)").arg(p.toArray().at(0).toDouble()).arg(p.toArray().at(1).toDouble());
                s += QStringLiteral("; its points %1").arg(said.join(QLatin1Char(' ')));
            }
        }
        lines << s;
    }
    if (!addressed.isEmpty())
        lines << QStringLiteral("(%1 stroke(s) marked addressed, left out: %2)").arg(addressed.size()).arg(addressed.join(QStringLiteral(", ")));
    if (!lines.isEmpty())
        lines << QStringLiteral("(In the geometry's unit, %1, y up; domains, boundaries and points by the numbers the model's selections use.)").arg(unit);
    return lines.join(QLatin1Char('\n'));
}

} // namespace qucs_s::fem
