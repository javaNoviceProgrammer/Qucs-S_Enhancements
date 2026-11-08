/*
 * layoutstrokes.cpp - the pen's strokes on a layout read against what is
 *                     there (layoutstrokes.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "layoutstrokes.h"

#include "strokeshape.h"

#include <QJsonObject>
#include <QLineF>
#include <QMap>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace qucs_s::layout {

namespace {

constexpr double Degrees = 180.0 / M_PI;

double dot(QPointF a, QPointF b) { return a.x() * b.x() + a.y() * b.y(); }
double cross(QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); }

/// The point of the segment from \a a to \a b nearest \a p.
QPointF nearestOn(QPointF p, QPointF a, QPointF b)
{
    const QPointF d = b - a;
    const double l2 = dot(d, d);
    const double t = l2 > 0 ? std::clamp(dot(p - a, d) / l2, 0.0, 1.0) : 0.0;
    return a + t * d;
}

/// A shape or a text near a stroke: its outline (a text's: its origin)
/// and that outline's box.
struct Near {
    Found found;
    QPolygonF outline;
    QRectF box;
};

/// What \a base finds whose bounds meet \a region, MostRead at most.
QList<Near> nearOf(const Layout& layout, Query base, const QRectF& region)
{
    QList<Near> out;
    base.region = region;
    search(layout, base, [&layout, &out](const Found& f) {
        const QPolygonF outline = outlineOf(layout, f);
        out << Near{f, outline, outline.boundingRect()};
        return out.size() < MostRead;
    });
    return out;
}

/// How far \a point lies from \a n: nought inside it.
double distanceTo(const Near& n, QPointF point)
{
    const QPolygonF& o = n.outline;
    if (o.isEmpty()) return std::numeric_limits<double>::infinity();
    if (n.found.isText() || o.size() == 1) return QLineF(point, o.first()).length();
    if (o.containsPoint(point, Qt::OddEvenFill)) return 0;
    double d = std::numeric_limits<double>::infinity();
    for (int i = 0; i < o.size(); ++i) d = std::min(d, QLineF(point, nearestOn(point, o.at(i), o.at((i + 1) % o.size()))).length());
    return d;
}

/// A shape or a text, once: where it is defined and which copy, placed.
QString keyOf(const Found& f)
{
    return QStringLiteral("%1 %2 %3 %4 %5 %6").arg(f.cell).arg(f.shape).arg(f.label).arg(f.copy).arg(f.transform.dx()).arg(f.transform.dy());
}

/// What a reading of a layout gathers: the shapes by layer, the texts, the
/// cells placed - each once, and none of those set \a apart.
struct Gathered {
    QMap<QString, int> shapes, cells;
    QStringList texts;
    QSet<QString> seen;
    const QSet<QString>* apart = nullptr;

    bool isEmpty() const { return seen.isEmpty() && cells.isEmpty(); }
    QJsonObject json() const
    {
        QJsonObject o;
        if (!shapes.isEmpty()) {
            QJsonObject s;
            for (auto it = shapes.begin(); it != shapes.end(); ++it) s.insert(it.key(), it.value());
            o.insert(QStringLiteral("shapes"), s);
        }
        if (!texts.isEmpty()) o.insert(QStringLiteral("texts"), QJsonArray::fromStringList(texts));
        if (!cells.isEmpty()) {
            QJsonObject c;
            for (auto it = cells.begin(); it != cells.end(); ++it) c.insert(it.key(), it.value());
            o.insert(QStringLiteral("cells"), c);
        }
        return o;
    }
    bool isApart(const Found& f) const { return apart != nullptr && apart->contains(keyOf(f)); }
    /// A shape or a text found, counted once.
    void add(const Layout& layout, const Found& f)
    {
        const QString key = keyOf(f);
        if (seen.contains(key) || (apart != nullptr && apart->contains(key))) return;
        seen.insert(key);
        if (f.isText()) {
            const QString text = layout.cells.at(f.cell).labels.at(f.label).text;
            if (!texts.contains(text)) texts << text;
            return;
        }
        ++shapes[layout.layers.at(layout.cells.at(f.cell).shapes.at(f.shape).layer).key.text()];
    }
};

/// Calls \a each with every copy's box of each cell \a cell places
/// (its own coordinates) that meets \a region, and the placed cell.
void eachPlaced(const Layout& layout, int cell, const QRectF& region, const std::function<void(int, const QRectF&)>& each)
{
    const Cell& c = layout.cells.at(cell);
    for (const Instance& inst : c.instances) {
        if (inst.cell < 0) continue;
        const QRectF one = inst.transform().mapRect(layout.cells.at(inst.cell).bounds);
        if (inst.repetition < 0) {
            if (one.intersects(region)) each(inst.cell, one);
            continue;
        }
        c.repetitions.at(inst.repetition).forEach(one, region, [&each, &inst, &one](QPointF off) {
            each(inst.cell, one.translated(off));
            return true;
        });
    }
}

/// The placements \a point lies in, into \a g.
void placedAt(const Layout& layout, int cell, QPointF point, Gathered& g)
{
    eachPlaced(layout, cell, QRectF(point - QPointF(1e-6, 1e-6), QSizeF(2e-6, 2e-6)), [&layout, &g, point](int placed, const QRectF& box) {
        if (box.contains(point)) ++g.cells[layout.cells.at(placed).name];
    });
}

/// What \a ring holds half inside it or more, of what \a base finds: a
/// text, its origin.
QJsonObject gatheredWithin(const Layout& layout, const Query& base, const QPolygonF& ring, const QSet<QString>* apart)
{
    Gathered g;
    g.apart = apart;
    for (const Near& n : nearOf(layout, base, ring.boundingRect()))
        if (n.found.isText() ? ring.containsPoint(n.outline.value(0), Qt::OddEvenFill) : markup::insideShare(ring, n.box) >= 0.5)
            g.add(layout, n.found);
    eachPlaced(layout, base.cell, ring.boundingRect(), [&layout, &ring, &g](int placed, const QRectF& box) {
        if (markup::insideShare(ring, box) >= 0.5) ++g.cells[layout.cells.at(placed).name];
    });
    return g.json();
}

/// What lies under \a along - within \a reach of any of its points - of
/// what \a base finds, and the placements it passes through; the shapes
/// under it into \a under.
QJsonObject gatheredUnder(const Layout& layout, const Query& base, const QPolygonF& along, double reach, const QSet<QString>* apart,
                          QList<Near>* under = nullptr)
{
    Gathered g;
    g.apart = apart;
    const QRectF region = along.boundingRect().adjusted(-reach, -reach, reach, reach);
    for (const Near& n : nearOf(layout, base, region)) {
        const QRectF grown = n.box.adjusted(-reach, -reach, reach, reach);
        for (const QPointF& pt : along)
            if (grown.contains(pt) && distanceTo(n, pt) <= reach) {
                if (under != nullptr && !n.found.isText() && !g.isApart(n.found)) *under << n;
                g.add(layout, n.found);
                break;
            }
    }
    eachPlaced(layout, base.cell, region, [&layout, &along, &g](int placed, const QRectF& box) {
        for (const QPointF& pt : along)
            if (box.contains(pt)) {
                ++g.cells[layout.cells.at(placed).name];
                break;
            }
    });
    return g.json();
}

/// What a stroke reaching \a end points at, of what \a base finds: what is
/// under it there (within \a close) and the placements it is in, else the
/// first such on ahead of it - \a along, within \a ahead - else the nearest
/// shape or text within \a reach of its end.
QJsonObject gatheredAt(const Layout& layout, const Query& base, QPointF end, QPointF along, double ahead, double close, double reach,
                       const QSet<QString>* apart)
{
    const double grow = std::max(close, reach);
    const QRectF region = QRectF(end, end + along * ahead).normalized().adjusted(-grow, -grow, grow, grow);
    QList<Near> near = nearOf(layout, base, region);
    if (apart != nullptr) near.removeIf([apart](const Near& n) { return apart->contains(keyOf(n.found)); });
    for (int k = 0; k <= (ahead > 0 ? 16 : 0); ++k) {
        const QPointF pt = end + along * (ahead * k / 16);
        Gathered g;
        for (const Near& n : near)
            if (n.box.adjusted(-close, -close, close, close).contains(pt) && distanceTo(n, pt) <= close) g.add(layout, n.found);
        // (The placements a point is in count only with something of
        //  them there, while large shapes are set apart: a cell's box is
        //  no more what is meant than a frame is.)
        if (apart == nullptr || !g.isEmpty()) placedAt(layout, base.cell, pt, g);
        if (!g.isEmpty()) return g.json();
    }
    // The nearest, and whatever is as near.
    double best = reach;
    for (const Near& n : near) best = std::min(best, distanceTo(n, end));
    Gathered g;
    if (best < reach)
        for (const Near& n : near)
            if (distanceTo(n, end) <= best + close) g.add(layout, n.found);
    return g.json();
}

// ----------------------------------------------------------------------
// What a stroke is about

/// The shapes much larger than \a s that lie under all of it - a frame, a
/// die, a fill, a well, or a pad under a dot -, the largest first.
QList<Near> largeUnder(const Layout& layout, const Query& base, const markup::Stroke& s)
{
    QList<Near> out;
    const QRectF b = s.bounds();
    const double area = std::max(b.width(), s.width) * std::max(b.height(), s.width);
    const QPolygonF few = markup::simplified(s.points, std::max(s.width / 2, 1e-9));
    for (const Near& n : nearOf(layout, base, b)) {
        if (n.found.isText() || !n.box.contains(b) || n.box.width() * n.box.height() < 8 * area) continue;
        if (std::all_of(few.begin(), few.end(), [&n](QPointF p) { return n.outline.containsPoint(p, Qt::OddEvenFill); })) out << n;
    }
    std::stable_sort(out.begin(), out.end(), [](const Near& a, const Near& b) { return a.box.width() * a.box.height() > b.box.width() * b.box.height(); });
    return out;
}

/// A shape's lines: a path's spine (and its half width), else its
/// outline, closed.
struct Lines {
    QPolygonF points;
    bool closed = false;
    double halfWidth = 0;
    bool path = false;
};

Lines linesOf(const Layout& layout, const Found& f)
{
    Lines l;
    const Shape& sh = layout.cells.at(f.cell).shapes.at(f.shape);
    if (sh.kind == ShapeKind::Path && sh.path >= 0) {
        l.points = spineOf(layout, f);
        l.path = l.points.size() >= 2;
        const double scale = std::sqrt(std::abs(f.transform.determinant()));
        l.halfWidth = layout.cells.at(f.cell).paths.at(sh.path).width * scale / 2;
    }
    if (!l.path) {
        l.points = outlineOf(layout, f);
        l.closed = true;
        l.halfWidth = 0;
    }
    return l;
}

/// "path", "polygon" or "text" (kindName() says polygon of a path too).
QString kindOf(const Layout& layout, const Found& f)
{
    if (f.isText()) return QStringLiteral("text");
    return layout.cells.at(f.cell).shapes.at(f.shape).kind == ShapeKind::Path ? QStringLiteral("path") : QStringLiteral("polygon");
}

/// What a feature belongs to: its layer, its kind, and the cell it is in
/// when that is not the cell read.
QJsonObject shapeRef(const Layout& layout, const Query& base, const Found& f)
{
    QJsonObject o{{QStringLiteral("layer"), layout.layers.at(layout.cells.at(f.cell).shapes.at(f.shape).layer).key.text()},
                  {QStringLiteral("kind"), kindOf(layout, f)}};
    if (f.cell != base.cell) o.insert(QStringLiteral("in"), layout.cells.at(f.cell).name);
    return o;
}

/// A value to \a step, and no -0.
double to(double v, double step)
{
    return std::round(std::round(v / step) * step * 1e6) / 1e6 + 0.0;
}

QJsonArray pointJson(QPointF p, double step)
{
    return {to(p.x(), step), to(p.y(), step)};
}

/// The angle between a corner's two edges, degrees.
double cornerAngle(QPointF before, QPointF at, QPointF after)
{
    const QPointF u = before - at, v = after - at;
    const double nu = std::hypot(u.x(), u.y()), nv = std::hypot(v.x(), v.y());
    if (nu <= 0 || nv <= 0) return 180;
    return std::acos(std::clamp(dot(u, v) / (nu * nv), -1.0, 1.0)) * Degrees;
}

/// The \a k'th vertex of \a l as a feature: a path's end or vertex, a
/// polygon's corner, with its angle.
QJsonObject vertexFeature(QJsonObject ref, const Lines& l, int k, double step)
{
    const int n = int(l.points.size());
    const QPointF at = l.points.at(k);
    if (l.path && (k == 0 || k == n - 1)) {
        ref.insert(QStringLiteral("end"), pointJson(at, step));
        return ref;
    }
    const QPointF before = l.points.at((k - 1 + n) % n), after = l.points.at((k + 1) % n);
    ref.insert(l.path ? QStringLiteral("vertex") : QStringLiteral("corner"), pointJson(at, step));
    ref.insert(QStringLiteral("angle"), std::round(cornerAngle(before, at, after)));
    return ref;
}

/// Where \a p lies from the line through \a a and \a b: above, below,
/// left or right of it.
QString sideOf(QPointF p, QPointF a, QPointF b)
{
    const QPointF d = b - a;
    if (std::abs(d.x()) >= std::abs(d.y())) {
        const double y = d.x() != 0 ? a.y() + (p.x() - a.x()) * d.y() / d.x() : a.y();
        return p.y() >= y ? QStringLiteral("above") : QStringLiteral("below");
    }
    const double x = a.x() + (p.y() - a.y()) * d.x() / d.y();
    return p.x() >= x ? QStringLiteral("right") : QStringLiteral("left");
}

/// The way \a from lies from \a to, as one says it: "the upper right".
QString compassOf(QPointF from, QPointF to)
{
    const QPointF d = from - to;
    if (std::hypot(d.x(), d.y()) <= 0) return QString();
    static const char* const names[] = {"the right", "the upper right", "above", "the upper left", "the left", "the lower left", "below", "the lower right"};
    double a = std::atan2(d.y(), d.x()) * Degrees;
    if (a < 0) a += 360;
    return QLatin1String(names[int(std::floor((a + 22.5) / 45)) % 8]);
}

/// The feature of what \a near holds (none set \a apart) nearest \a point,
/// within \a reach: a text; a vertex, an end or a corner; else a segment
/// or an edge, and which side of it \a point lies. A vertex is preferred
/// to an edge a little nearer (a path's half width, \a close more): an
/// arrow points at a corner from outside it.
std::optional<QJsonObject> featureNear(const Layout& layout, const Query& base, const QList<Near>& near, QPointF point, double reach, double close,
                                       const QSet<QString>& apart, double step)
{
    double bestVertex = reach, bestEdge = reach, bestText = reach;
    std::optional<QJsonObject> vertex, edge, text;
    for (const Near& n : near) {
        if (apart.contains(keyOf(n.found))) continue;
        if (n.found.isText()) {
            const double d = QLineF(point, n.outline.value(0)).length();
            if (d < bestText) {
                bestText = d;
                text = QJsonObject{{QStringLiteral("text"), layout.cells.at(n.found.cell).labels.at(n.found.label).text},
                                   {QStringLiteral("at"), pointJson(n.outline.value(0), step)}};
            }
            continue;
        }
        if (n.box.adjusted(-reach, -reach, reach, reach).contains(point) == false) continue;
        const Lines l = linesOf(layout, n.found);
        if (l.points.size() > 5000) continue;   // (a vast outline: its box is all a reading has of it)
        const QJsonObject ref = shapeRef(layout, base, n.found);
        const int count = int(l.points.size());
        const double allowance = l.halfWidth + close;
        for (int k = 0; k < count; ++k) {
            const double d = QLineF(point, l.points.at(k)).length() - allowance;
            if (d < bestVertex) {
                bestVertex = d;
                vertex = vertexFeature(ref, l, k, step);
            }
        }
        for (int k = 0; k + (l.closed ? 0 : 1) < count; ++k) {
            const QPointF a = l.points.at(k), b = l.points.at((k + 1) % count);
            const double d = QLineF(point, nearestOn(point, a, b)).length();
            if (d < bestEdge) {
                bestEdge = d;
                QJsonObject e = ref;
                e.insert(l.path ? QStringLiteral("segment") : QStringLiteral("edge"), QJsonArray{pointJson(a, step), pointJson(b, step)});
                e.insert(QStringLiteral("side"), sideOf(point, a, b));
                edge = e;
            }
        }
    }
    if (text && bestText <= std::min(bestVertex, bestEdge)) return text;
    if (vertex && bestVertex <= bestEdge) return vertex;
    return edge ? edge : vertex;
}

/// What part a ring holds of the large shapes it does not hold half of:
/// their vertices inside it, else the segments or edges through it.
QJsonArray partsWithin(const Layout& layout, const Query& base, const QPolygonF& ring, const QSet<QString>& apart, double step)
{
    QJsonArray out;
    int looked = 0;
    for (const Near& n : nearOf(layout, base, ring.boundingRect())) {
        if (n.found.isText() || apart.contains(keyOf(n.found)) || markup::insideShare(ring, n.box) >= 0.5) continue;
        if (++looked > 64) break;   // (a ring over a dense block: its first few dozen)
        const Lines l = linesOf(layout, n.found);
        if (l.points.size() > 5000) continue;
        const QJsonObject ref = shapeRef(layout, base, n.found);
        const int count = int(l.points.size());
        int found = 0;
        for (int k = 0; k < count && found < 4; ++k)
            if (ring.containsPoint(l.points.at(k), Qt::OddEvenFill)) {
                out.append(vertexFeature(ref, l, k, step));
                ++found;
            }
        for (int k = 0; found == 0 && k + (l.closed ? 0 : 1) < count; ++k) {
            const QPointF a = l.points.at(k), b = l.points.at((k + 1) % count);
            bool through = false;
            for (int j = 1; j < 16 && !through; ++j) through = ring.containsPoint(a + (b - a) * (j / 16.0), Qt::OddEvenFill);
            if (!through) continue;
            QJsonObject e = ref;
            e.insert(l.path ? QStringLiteral("segment") : QStringLiteral("edge"), QJsonArray{pointJson(a, step), pointJson(b, step)});
            out.append(e);
            if (out.size() >= 8) return out;
        }
        if (out.size() >= 8) break;
    }
    return out;
}

/// How \a along - a line's or a mark's middle line - follows \a l: from
/// where to where it keeps within \a near of it, and where it leaves it
/// between, to go how far from it and which way; none when it keeps to it
/// for less than a quarter of its length (a tenth, coming back to it).
std::optional<QJsonObject> followOf(const Lines& l, const QPolygonF& along, double near, double step)
{
    const int n = int(along.size()), count = int(l.points.size());
    if (n < 2 || count < 2) return std::nullopt;
    QList<double> at(n, 0);   // how far along
    QList<QPointF> foot(n);   // the nearest point of the shape's lines
    QList<bool> on(n, false);
    for (int i = 0; i < n; ++i) {
        if (i > 0) at[i] = at.at(i - 1) + QLineF(along.at(i - 1), along.at(i)).length();
        double best = std::numeric_limits<double>::infinity();
        QPointF way;
        for (int k = 0; k + (l.closed ? 0 : 1) < count; ++k) {
            const QPointF a = l.points.at(k), b = l.points.at((k + 1) % count);
            const QPointF q = nearestOn(along.at(i), a, b);
            if (const double d = QLineF(q, along.at(i)).length(); d < best) {
                best = d;
                foot[i] = q;
                way = b - a;
            }
        }
        // Along it, not across: the stroke here within 30° of its way
        // there (a vertex's either way).
        const QPointF here = along.at(std::min(i + 1, n - 1)) - along.at(std::max(i - 1, 0));
        const double lengths = std::hypot(here.x(), here.y()) * std::hypot(way.x(), way.y());
        on[i] = best <= near && lengths > 0 && std::abs(dot(here, way)) >= std::cos(30 / Degrees) * lengths;
    }
    const double total = at.last();
    if (!(total > 0)) return std::nullopt;
    struct Span {
        bool on;
        int a, b;
    };
    QList<Span> spans;
    for (int i = 0; i < n; ++i) {
        if (spans.isEmpty() || spans.last().on != on.at(i)) spans << Span{on.at(i), i, i};
        else spans.last().b = i;
    }
    const auto lengthOf = [&at, n](const Span& s) { return at.at(std::min(s.b + 1, n - 1)) - at.at(s.a); };
    // Brief touches and brief strays between are neither.
    const double least = std::max(0.05 * total, 2 * near);
    for (bool merged = true; merged && spans.size() > 2;) {
        merged = false;
        int shortest = -1;
        for (int i = 1; i + 1 < spans.size(); ++i)
            if (lengthOf(spans.at(i)) < least && (shortest < 0 || lengthOf(spans.at(i)) < lengthOf(spans.at(shortest)))) shortest = i;
        if (shortest < 0) break;
        spans[shortest - 1].b = spans.at(shortest + 1).b;
        spans.remove(shortest, 2);
        merged = true;
    }
    // Along it a quarter of its length at least - or a tenth, leaving it
    // and coming back: a detour from it.
    double kept = 0;
    int ons = 0;
    for (const Span& s : spans)
        if (s.on) {
            kept += lengthOf(s);
            ++ons;
        }
    if (kept < (ons >= 2 ? 0.1 : 0.25) * total) return std::nullopt;
    int first = -1, last = -1;
    for (int i = 0; i < spans.size(); ++i)
        if (spans.at(i).on) {
            if (first < 0) first = i;
            last = i;
        }
    QJsonObject o{{QStringLiteral("from"), pointJson(foot.at(spans.at(first).a), step)}, {QStringLiteral("to"), pointJson(foot.at(spans.at(last).b), step)}};
    QJsonArray leaves;
    for (int i = first + 1; i < last; ++i) {
        if (spans.at(i).on) continue;
        const QPointF a = foot.at(spans.at(i - 1).b), b = foot.at(spans.at(i + 1).a);
        const QPointF chord = b - a;
        const double length = std::hypot(chord.x(), chord.y());
        double most = -1;
        QPointF far;
        for (int k = spans.at(i).a; k <= spans.at(i).b; ++k) {
            const QPointF q = along.at(k);
            const double d = length > 0 ? std::abs(cross(chord, q - a)) / length : QLineF(q, a).length();
            if (d > most) {
                most = d;
                far = q;
            }
        }
        leaves.append(QJsonObject{{QStringLiteral("at"), pointJson(a, step)},
                                  {QStringLiteral("back"), pointJson(b, step)},
                                  {QStringLiteral("by"), to(most, step)},
                                  {QStringLiteral("side"), length > 0 ? sideOf(far, a, b) : QStringLiteral("off")}});
    }
    if (!leaves.isEmpty()) o.insert(QStringLiteral("leaves"), leaves);
    return o;
}

/// What a polyline drawn near \a near (none set \a apart) may snap to: its
/// paths' spines and edges, its polygons' corners and edges, its texts.
markup::Snaps snapsNear(const Layout& layout, const QList<Near>& near, const QSet<QString>& apart)
{
    markup::Snaps snaps;
    snaps.finest = layout.dbu > 0 ? layout.dbu : 0.001;
    QSet<qint64> xs, ys;   // (each value once, the first named kept)
    const auto add = [&snaps, &xs, &ys](bool across, double v, const QString& what) {
        QSet<qint64>& seen = across ? xs : ys;
        const qint64 key = std::llround(v * 1e6);
        if (seen.contains(key)) return;
        seen.insert(key);
        (across ? snaps.xs : snaps.ys) << markup::Snap{v, what};
    };
    for (int pass = 0; pass < 4; ++pass)
        for (const Near& n : near) {
            if (apart.contains(keyOf(n.found))) continue;
            if (n.found.isText()) {
                if (pass == 2) {
                    const QString what = QStringLiteral("the text %1").arg(layout.cells.at(n.found.cell).labels.at(n.found.label).text);
                    add(true, n.outline.value(0).x(), what);
                    add(false, n.outline.value(0).y(), what);
                }
                continue;
            }
            const Lines l = linesOf(layout, n.found);
            if (l.points.size() > 2000) continue;
            const QString name = QStringLiteral("the %1 %2's").arg(layout.layers.at(layout.cells.at(n.found.cell).shapes.at(n.found.shape).layer).key.text(), kindOf(layout, n.found));
            const int count = int(l.points.size());
            // First a path's centre lines, then its vertices, then a
            // polygon's corners and the texts, then the edges.
            if (pass == 0 && l.path)
                for (int k = 1; k < count; ++k) {
                    const QPointF a = l.points.at(k - 1), b = l.points.at(k);
                    if (std::abs(a.y() - b.y()) < 1e-9) add(false, a.y(), name + QStringLiteral(" centre line"));
                    if (std::abs(a.x() - b.x()) < 1e-9) add(true, a.x(), name + QStringLiteral(" centre line"));
                }
            if ((pass == 1 && l.path) || (pass == 2 && !l.path))
                for (int k = 0; k < count; ++k) {
                    const QString what = l.path ? (k == 0 || k == count - 1 ? name + QStringLiteral(" end") : name + QStringLiteral(" vertex"))
                                                : name + QStringLiteral(" corner");
                    add(true, l.points.at(k).x(), what + QStringLiteral(" at ") + markup::pointSaid(l.points.at(k)));
                    add(false, l.points.at(k).y(), what + QStringLiteral(" at ") + markup::pointSaid(l.points.at(k)));
                }
            if (pass == 3) {
                const QPolygonF o = n.outline;
                for (int k = 0; k < o.size() && o.size() <= 2000; ++k) {
                    const QPointF a = o.at(k), b = o.at((k + 1) % o.size());
                    if (std::abs(a.y() - b.y()) < 1e-9) add(false, a.y(), name + QStringLiteral(" edge"));
                    if (std::abs(a.x() - b.x()) < 1e-9) add(true, a.x(), name + QStringLiteral(" edge"));
                }
            }
        }
    return snaps;
}

/// Whether a reading says what its stroke marks: anything beyond how it
/// was drawn.
bool marksSomething(const QJsonObject& o)
{
    for (const char* key : {"around", "on", "highlights", "from", "points at", "at", "holds part of", "nearest its tip", "follows"})
        if (o.contains(QLatin1String(key))) return true;
    return false;
}

/// One stroke's reading, the shapes in \a apart left out of what it marks.
QJsonObject readOne(const Layout& layout, const Query& base, const markup::Stroke& s, markup::Gesture g, QPointF tip, const QSet<QString>& apart,
                    QJsonObject o)
{
    const QSet<QString>* away = apart.isEmpty() ? nullptr : &apart;
    const double reach = markup::reachOf(s), close = std::max(s.width / 2, 1e-9);
    // Points said to a tenth of the pen's breadth, or so.
    const double step = std::pow(10.0, std::floor(std::log10(std::max(s.width / 2, layout.dbu > 0 ? layout.dbu : 1e-3))));
    const double size = QLineF(s.points.boundingRect().topLeft(), s.points.boundingRect().bottomRight()).length();
    switch (g) {
    case markup::Gesture::Arrowhead: break;
    case markup::Gesture::Dot:
        if (const QJsonObject on = gatheredAt(layout, base, s.points.first(), QPointF(), 0, close, reach, away); !on.isEmpty())
            o.insert(QStringLiteral("on"), on);
        break;
    case markup::Gesture::Corner:
        // What its point is at.
        if (const QJsonObject at = gatheredAt(layout, base, tip, QPointF(), 0, close, reach, away); !at.isEmpty()) o.insert(QStringLiteral("at"), at);
        o.insert(QStringLiteral("point"), QJsonArray{std::round(tip.x() * 1000) / 1000, std::round(tip.y() * 1000) / 1000});
        break;
    case markup::Gesture::Ring: {
        QPolygonF ring = s.points;
        ring << ring.first();
        if (const QJsonObject in = gatheredWithin(layout, base, ring, away); !in.isEmpty()) o.insert(QStringLiteral("around"), in);
        if (const QJsonArray parts = partsWithin(layout, base, ring, apart, step); !parts.isEmpty()) o.insert(QStringLiteral("holds part of"), parts);
        break;
    }
    case markup::Gesture::Line:
    case markup::Gesture::Arrow:
    case markup::Gesture::Mark: {
        // A highlighter's stroke, or a mark: what it lies on.
        QList<Near> under;
        const QPolygonF along = markup::alongOf(s);
        if (s.highlighter || g == markup::Gesture::Mark) {
            if (const QJsonObject on = gatheredUnder(layout, base, along, close, away, &under); !on.isEmpty())
                o.insert(s.highlighter ? QStringLiteral("highlights") : QStringLiteral("on"), on);
        }
        if (g != markup::Gesture::Arrow) {
            // Along a shape: where it follows it, and where it leaves it.
            if (under.isEmpty()) gatheredUnder(layout, base, along, close, away, &under);
            // (Paths first, and two dozen shapes at most: a mark over a
            //  dense block is not compared with each of thousands.)
            std::stable_partition(under.begin(), under.end(), [&layout](const Near& n) {
                return layout.cells.at(n.found.cell).shapes.at(n.found.shape).kind == ShapeKind::Path;
            });
            if (under.size() > 24) under.resize(24);
            QJsonArray follows;
            for (const Near& n : std::as_const(under)) {
                if (follows.size() >= 4) break;
                const Lines l = linesOf(layout, n.found);
                if (l.points.size() > 2000) continue;
                if (std::optional<QJsonObject> f = followOf(l, along, l.halfWidth + std::max(s.width, 0.02 * size), step)) {
                    QJsonObject ref = shapeRef(layout, base, n.found);
                    for (auto it = f->begin(); it != f->end(); ++it) ref.insert(it.key(), it.value());
                    follows.append(ref);
                }
            }
            if (!follows.isEmpty()) o.insert(QStringLiteral("follows"), follows);
        }
        if ((s.highlighter || g == markup::Gesture::Mark) && g != markup::Gesture::Arrow) break;
        // Else from something, to what it points at.
        const QPointF start = s.points.first(), end = tip;
        const double length = QLineF(start, end).length();
        const QPointF way = length > 0 ? (end - start) / length : QPointF();
        if (const QJsonObject from = gatheredAt(layout, base, start, -way, 0, close, reach, away); !from.isEmpty())
            o.insert(QStringLiteral("from"), from);
        if (const QJsonObject at = gatheredAt(layout, base, end, way, 0.3 * length, close, reach, away); !at.isEmpty())
            o.insert(QStringLiteral("points at"), at);
        if (g == markup::Gesture::Arrow) {
            // Its tip and tail, the way it comes from, and the feature its
            // tip is nearest.
            const QPointF tail = QLineF(tip, s.points.first()).length() >= QLineF(tip, s.points.last()).length() ? s.points.first() : s.points.last();
            o.insert(QStringLiteral("tip"), pointJson(tip, step));
            o.insert(QStringLiteral("tail"), pointJson(tail, step));
            if (const QString from = compassOf(tail, tip); !from.isEmpty()) o.insert(QStringLiteral("comes from"), from);
            const double within = std::max(reach, 0.3 * QLineF(tail, tip).length());
            const QList<Near> near = nearOf(layout, base, QRectF(tip, tip).adjusted(-within, -within, within, within));
            if (std::optional<QJsonObject> f = featureNear(layout, base, near, tip, within, reach, apart, step)) o.insert(QStringLiteral("nearest its tip"), *f);
        }
        break;
    }
    }
    return o;
}

} // namespace

QJsonArray readStrokes(const Layout& layout, const Query& base, const QList<markup::Stroke>& strokes, bool points)
{
    QJsonArray out;
    if (base.cell < 0 || base.cell >= layout.cells.size()) return out;
    const QString part = layout.cells.at(base.cell).name;
    QList<QPointF> tips;
    QList<int> heads;
    const QList<markup::Gesture> gestures = markup::gesturesOf(strokes, &tips, &heads);
    for (int i = 0; i < strokes.size(); ++i) {
        const markup::Stroke& s = strokes.at(i);
        const markup::Gesture g = gestures.at(i);
        if (s.points.isEmpty()) continue;
        QJsonObject o = markup::readingOf(s, i + 1, g, points);
        o.insert(QStringLiteral("cell"), part);
        if (g == markup::Gesture::Arrowhead) {
            const markup::Stroke& shaft = strokes.at(heads.at(i));
            o.insert(QStringLiteral("of stroke"), shaft.id > 0 ? shaft.id : heads.at(i) + 1);
            out.append(o);
            continue;
        }
        // The shapes much larger than the stroke under all of it are set
        // apart, the largest first, while it marks something besides: a
        // dot on a pad on a die is on the pad; one on the die alone, on the
        // die.
        const QList<Near> large = largeUnder(layout, base, s);
        QSet<QString> apart;
        QJsonObject reading;
        for (int kept = 0; kept <= large.size(); ++kept) {
            apart.clear();
            Gathered over;
            for (int k = 0; k < large.size() - kept; ++k) {
                apart.insert(keyOf(large.at(k).found));
                over.add(layout, large.at(k).found);
            }
            reading = readOne(layout, base, s, g, tips.at(i), apart, o);
            if (apart.isEmpty()) break;
            if (marksSomething(reading)) {
                reading.insert(QStringLiteral("over"), over.json());
                break;
            }
        }
        // A freehand stroke as a polyline, snapped to what is near it.
        if (g == markup::Gesture::Line || g == markup::Gesture::Corner || g == markup::Gesture::Mark || g == markup::Gesture::Ring) {
            const QRectF b = s.points.boundingRect();
            const double grow = std::max(2 * s.width, 0.02 * std::hypot(b.width(), b.height()));
            const QList<Near> near = nearOf(layout, base, b.adjusted(-grow, -grow, grow, grow));
            // (Worth saying with a corner at least, or a level or upright
            //  line drawn by hand: a slanted one's points say as much.)
            std::optional<markup::Polyline> pl = markup::polylineOf(s.points, s.width, g == markup::Gesture::Ring, snapsNear(layout, near, apart));
            if (pl && (pl->runs.size() >= 2
                       || (s.points.size() > 2 && (pl->runs.first().way == markup::Polyline::Way::Horizontal || pl->runs.first().way == markup::Polyline::Way::Vertical))))
                reading.insert(QStringLiteral("polyline"), markup::polylineJson(*pl));
        }
        out.append(reading);
    }
    return out;
}

} // namespace qucs_s::layout
