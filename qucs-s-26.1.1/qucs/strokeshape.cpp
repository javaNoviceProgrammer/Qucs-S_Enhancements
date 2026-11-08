/*
 * strokeshape.cpp - a freehand stroke read as a polyline (strokeshape.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "strokeshape.h"

#include <QJsonArray>
#include <QLineF>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace qucs_s::markup {

namespace {

constexpr double Degrees = 180.0 / M_PI;

double dot(QPointF a, QPointF b) { return a.x() * b.x() + a.y() * b.y(); }
double cross(QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); }
double norm(QPointF a) { return std::hypot(a.x(), a.y()); }

double segmentDistance(QPointF p, QPointF a, QPointF b)
{
    const QPointF d = b - a;
    const double l2 = dot(d, d);
    const double t = l2 > 0 ? std::clamp(dot(p - a, d) / l2, 0.0, 1.0) : 0.0;
    return norm(a + t * d - p);
}

/// The points Douglas and Peucker's simplification keeps of p[a..b].
QList<int> keptOf(const QPolygonF& p, int a, int b, double tolerance)
{
    QList<bool> keep(b - a + 1, false);
    keep.first() = keep.last() = true;
    QList<std::pair<int, int>> todo{{a, b}};
    while (!todo.isEmpty()) {
        const auto [from, to] = todo.takeLast();
        int far = -1;
        double most = tolerance;
        for (int i = from + 1; i < to; ++i)
            if (const double d = segmentDistance(p.at(i), p.at(from), p.at(to)); d > most) {
                most = d;
                far = i;
            }
        if (far >= 0) {
            keep[far - a] = true;
            todo << std::make_pair(from, far) << std::make_pair(far, to);
        }
    }
    QList<int> out;
    for (int i = 0; i < keep.size(); ++i)
        if (keep.at(i)) out << a + i;
    return out;
}

/// The turn from the way of \a u to that of \a v, degrees (0: straight on).
double turnOf(QPointF u, QPointF v)
{
    const double nu = norm(u), nv = norm(v);
    if (nu <= 0 || nv <= 0) return 0;
    return std::acos(std::clamp(dot(u, v) / (nu * nv), -1.0, 1.0)) * Degrees;
}

struct Segment {
    int a = 0, b = 0;       // its points, p[a..b]
};

/// A run fitted: a line through \a c the way \a d (a unit vector, the way it
/// was drawn), and the points it was fitted to, p[first..last].
struct Fit {
    QPointF c, d;
    Polyline::Run run;
    int first = 0, last = 0;
    bool curved = false;
};

Fit fitRun(const QPolygonF& p, const Segment& s, double tolerance, double width)
{
    Fit f;
    const QPointF A = p.at(s.a), B = p.at(s.b);
    const double length = norm(B - A);
    // Its points away from its corners, where a hand turns early or late.
    const double trim = std::min(std::max(1.5 * tolerance, 0.1 * length), 0.3 * length);
    QList<int> used;
    for (int i = s.a; i <= s.b; ++i)
        if (norm(p.at(i) - A) >= trim && norm(p.at(i) - B) >= trim) used << i;
    if (used.size() < 3) {
        used.clear();
        for (int i = s.a; i <= s.b; ++i) used << i;
    }
    f.first = used.first();
    f.last = used.last();
    QPointF c(0, 0);
    for (const int i : used) c += p.at(i);
    c /= double(used.size());
    double sxx = 0, sxy = 0, syy = 0;
    for (const int i : used) {
        const QPointF q = p.at(i) - c;
        sxx += q.x() * q.x();
        sxy += q.x() * q.y();
        syy += q.y() * q.y();
    }
    const double theta = 0.5 * std::atan2(2 * sxy, sxx - syy);
    QPointF d(std::cos(theta), std::sin(theta));
    if (dot(d, B - A) < 0) d = -d;
    // Curved? A parabola across it, against the hand's wobble.
    {
        double S[5] = {0, 0, 0, 0, 0}, T[3] = {0, 0, 0}, half = 0;
        for (const int i : used) {
            const QPointF q = p.at(i) - c;
            const double u = dot(q, d), v = cross(d, q);
            double un = 1;
            for (double& k : S) {
                k += un;
                un *= u;
            }
            T[0] += v;
            T[1] += u * v;
            T[2] += u * u * v;
            half = std::max(half, std::abs(u));
        }
        const auto det3 = [](double a, double b, double c, double d, double e, double f, double g, double h, double i) {
            return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        };
        const double D = det3(S[0], S[1], S[2], S[1], S[2], S[3], S[2], S[3], S[4]);
        if (used.size() >= 6 && std::abs(D) > 1e-12) {
            const double a0 = det3(T[0], S[1], S[2], T[1], S[2], S[3], T[2], S[3], S[4]) / D;
            const double a1 = det3(S[0], T[0], S[2], S[1], T[1], S[3], S[2], T[2], S[4]) / D;
            const double a2 = det3(S[0], S[1], T[0], S[1], S[2], T[1], S[2], S[3], T[2]) / D;
            double sum = 0;
            for (const int i : used) {
                const QPointF q = p.at(i) - c;
                const double u = dot(q, d), r = cross(d, q) - (a0 + a1 * u + a2 * u * u);
                sum += r * r;
            }
            const double sigma = std::sqrt(sum / std::max<double>(1, used.size() - 3));
            f.curved = std::abs(a2) * half * half > std::max({3 * sigma, 0.5 * tolerance, 0.25 * width});
        }
    }
    Polyline::Run& r = f.run;
    r.angle = std::atan2(d.y(), d.x()) * Degrees;
    const double a180 = std::fmod(r.angle + 360, 180);
    constexpr double Near = 7;   // degrees a hand's straight line may lean
    double spread = 0;
    if (std::min(a180, 180 - a180) <= Near) {
        r.way = Polyline::Way::Horizontal;
        d = QPointF(d.x() >= 0 ? 1 : -1, 0);
        r.at = c.y();
        for (const int i : used) spread = std::max(spread, std::abs(p.at(i).y() - c.y()));
    } else if (std::abs(a180 - 90) <= Near) {
        r.way = Polyline::Way::Vertical;
        d = QPointF(0, d.y() >= 0 ? 1 : -1);
        r.at = c.x();
        for (const int i : used) spread = std::max(spread, std::abs(p.at(i).x() - c.x()));
    } else {
        if (std::abs(a180 - 45) <= Near || std::abs(a180 - 135) <= Near) {
            r.way = Polyline::Way::Diagonal;
            d = QPointF(d.x() >= 0 ? 1 : -1, d.y() >= 0 ? 1 : -1) / std::sqrt(2.0);
        }
        for (const int i : used) spread = std::max(spread, std::abs(cross(d, p.at(i) - c)));
    }
    r.spread = spread;
    f.c = c;
    f.d = d;
    return f;
}

bool meet(QPointF c1, QPointF d1, QPointF c2, QPointF d2, QPointF* at)
{
    const double k = cross(d1, d2);
    if (std::abs(k) < 1e-9) return false;
    *at = c1 + d1 * (cross(c2 - c1, d2) / k);
    return true;
}

QPointF onLine(QPointF p, QPointF c, QPointF d)
{
    return c + d * dot(p - c, d);
}

/// The circle nearest \a p (Kåsa's): how far its points lie from it, rms.
double circleMiss(const QPolygonF& p)
{
    // x² + y² + D x + E y + F = 0, least squares, about the points' middle.
    QPointF m(0, 0);
    for (const QPointF& q : p) m += q;
    m /= double(p.size());
    double a[3][4] = {};
    for (const QPointF& q0 : p) {
        const QPointF q = q0 - m;
        const double row[3] = {q.x(), q.y(), 1}, z = -(q.x() * q.x() + q.y() * q.y());
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) a[i][j] += row[i] * row[j];
            a[i][3] += row[i] * z;
        }
    }
    for (int col = 0; col < 3; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 3; ++r)
            if (std::abs(a[r][col]) > std::abs(a[pivot][col])) pivot = r;
        if (std::abs(a[pivot][col]) < 1e-12) return std::numeric_limits<double>::infinity();
        for (int j = 0; j < 4; ++j) std::swap(a[col][j], a[pivot][j]);
        for (int r = 0; r < 3; ++r) {
            if (r == col) continue;
            const double k = a[r][col] / a[col][col];
            for (int j = col; j < 4; ++j) a[r][j] -= k * a[col][j];
        }
    }
    const double D = a[0][3] / a[0][0], E = a[1][3] / a[1][1], F = a[2][3] / a[2][2];
    const QPointF centre(-D / 2, -E / 2);
    const double r2 = D * D / 4 + E * E / 4 - F;
    if (!(r2 > 0)) return std::numeric_limits<double>::infinity();
    const double r = std::sqrt(r2);
    double sum = 0;
    for (const QPointF& q : p) {
        const double e = norm(q - m - centre) - r;
        sum += e * e;
    }
    return std::sqrt(sum / p.size());
}

double polylineMiss(const QPolygonF& p, const QPolygonF& corners, bool closed, double* most)
{
    double sum = 0;
    *most = 0;
    for (const QPointF& q : p) {
        double d = std::numeric_limits<double>::infinity();
        for (int k = 1; k < corners.size(); ++k) d = std::min(d, segmentDistance(q, corners.at(k - 1), corners.at(k)));
        if (closed && corners.size() > 2) d = std::min(d, segmentDistance(q, corners.last(), corners.first()));
        sum += d * d;
        *most = std::max(*most, d);
    }
    return std::sqrt(sum / std::max<qsizetype>(1, p.size()));
}

/// \a value snapped: to the nearest of \a near within \a reach, else to a
/// round grid within \a tolerance; what it snapped to in \a what.
double snapped(double value, const QList<Snap>& near, double reach, double tolerance, double finest, QString* what)
{
    const Snap* best = nullptr;
    for (const Snap& s : near)
        if (std::abs(s.value - value) <= reach && (best == nullptr || std::abs(s.value - value) < std::abs(best->value - value))) best = &s;
    if (best != nullptr) {
        *what = best->what;
        return best->value;
    }
    double step = 0;
    const double v = onRoundGrid(value, tolerance, finest, &step);
    *what = QStringLiteral("a %1 µm grid").arg(numberSaid(step));
    return v;
}

QString wayName(Polyline::Way way)
{
    switch (way) {
    case Polyline::Way::Horizontal: return QStringLiteral("horizontal");
    case Polyline::Way::Vertical: return QStringLiteral("vertical");
    case Polyline::Way::Diagonal: return QStringLiteral("diagonal");
    case Polyline::Way::Slanted: break;
    }
    return QStringLiteral("slanted");
}

double roundedTo(double v, double step)
{
    const double r = std::round(v / step) * step;
    return std::round(r * 1e6) / 1e6 + 0.0;   // (no -0)
}

QJsonArray pointJson(QPointF p, double step)
{
    return {roundedTo(p.x(), step), roundedTo(p.y(), step)};
}

QPointF pointOf(const QJsonValue& v)
{
    const QJsonArray a = v.toArray();
    return QPointF(a.at(0).toDouble(), a.at(1).toDouble());
}

} // namespace

QString numberSaid(double value)
{
    const double r = std::round(value * 1000) / 1000;
    QString s = QString::number(r == 0 ? 0.0 : r, 'f', 3);
    while (s.endsWith(QLatin1Char('0'))) s.chop(1);
    if (s.endsWith(QLatin1Char('.'))) s.chop(1);
    return s;
}

QString pointSaid(QPointF point)
{
    return QStringLiteral("(%1, %2)").arg(numberSaid(point.x()), numberSaid(point.y()));
}

double onRoundGrid(double value, double tolerance, double finest, double* step)
{
    static const double steps[] = {100, 50, 10, 5, 1, 0.5, 0.1, 0.05, 0.01, 0.005, 0.001, 0.0005, 0.0001};
    for (const double s : steps) {
        if (s < finest * (1 - 1e-9)) break;
        const double v = std::round(value / s) * s;
        if (std::abs(v - value) <= tolerance) {
            if (step != nullptr) *step = s;
            return v;
        }
    }
    const double s = finest > 0 ? finest : 0.001;
    if (step != nullptr) *step = s;
    return std::round(value / s) * s;
}

std::optional<Polyline> polylineOf(const QPolygonF& points, double width, bool closed, const Snaps& snaps)
{
    if (points.size() < 2) return std::nullopt;
    QPolygonF p = points;
    const QRectF box = p.boundingRect();
    const double size = std::hypot(box.width(), box.height());
    if (!(size > 3 * width) || !std::isfinite(size)) return std::nullopt;
    const double tolerance = std::max(1.5 * width, 0.03 * size);
    if (closed) {
        // Begun at a corner: the point furthest from the middle.
        const QPointF m = box.center();
        int far = 0;
        for (int i = 1; i < p.size(); ++i)
            if (norm(p.at(i) - m) > norm(p.at(far) - m)) far = i;
        std::rotate(p.begin(), p.begin() + far, p.end());
        p << p.first();
    }
    const int n = int(p.size());
    const QList<int> kept = keptOf(p, 0, n - 1, tolerance);
    QList<Segment> segs;
    for (int i = 1; i < kept.size(); ++i) segs << Segment{kept.at(i - 1), kept.at(i)};
    const auto lengthOf = [&p](const Segment& s) { return norm(p.at(s.b) - p.at(s.a)); };
    const auto wayOf = [&p](const Segment& s) { return p.at(s.b) - p.at(s.a); };
    // Runs from the segments: those straight on joined; a short one between
    // two long ones that turn a corner its rounding; a short hook where the
    // pen came down or lifted, dropped.
    for (bool changed = true; changed && !segs.isEmpty();) {
        changed = false;
        for (int i = 1; i < segs.size(); ++i)
            if (turnOf(wayOf(segs.at(i - 1)), wayOf(segs.at(i))) < 12) {
                segs[i - 1].b = segs.at(i).b;
                segs.removeAt(i);
                changed = true;
                break;
            }
        if (changed) continue;
        for (int i = 1; i + 1 < segs.size(); ++i) {
            const double l = lengthOf(segs.at(i));
            if (l < 3 * tolerance && lengthOf(segs.at(i - 1)) >= 2 * l && lengthOf(segs.at(i + 1)) >= 2 * l
                && turnOf(wayOf(segs.at(i - 1)), wayOf(segs.at(i + 1))) >= 20) {
                segs.removeAt(i);   // (its points lie between its neighbours: the corner's)
                changed = true;
                break;
            }
        }
        if (changed) continue;
        if (!closed && segs.size() >= 3) {
            if (lengthOf(segs.first()) < 2 * tolerance) {
                segs.removeFirst();
                changed = true;
            } else if (lengthOf(segs.last()) < 2 * tolerance) {
                segs.removeLast();
                changed = true;
            }
        }
    }
    if (segs.isEmpty() || segs.size() > (closed ? 6 : 12) || (closed && segs.size() < 3)) return std::nullopt;
    if (closed && turnOf(wayOf(segs.last()), wayOf(segs.first())) < 12) return std::nullopt;   // (not begun at a corner after all)

    QList<Fit> fits;
    for (const Segment& s : std::as_const(segs)) {
        fits << fitRun(p, s, tolerance, width);
        if (fits.last().curved) return std::nullopt;
    }
    const int runs = int(fits.size());
    Polyline out;
    out.closed = closed;
    for (const Fit& f : std::as_const(fits)) out.runs << f.run;

    // Its corners as drawn: where its runs' lines meet, and its ends.
    const auto corner = [&](int k, const QList<QPointF>& c, const QList<QPointF>& d, QPointF* at) {
        const int before = (k - 1 + runs) % runs;
        return meet(c.at(before), d.at(before), c.at(k), d.at(k), at);
    };
    QList<QPointF> cs, ds;
    for (const Fit& f : std::as_const(fits)) {
        cs << f.c;
        ds << f.d;
    }
    const QPointF start = p.at(segs.first().a), end = p.at(segs.last().b);
    if (!closed) out.drawn << onLine(start, cs.first(), ds.first());
    for (int k = closed ? 0 : 1; k < runs; ++k) {
        QPointF at;
        if (!corner(k, cs, ds, &at)) return std::nullopt;
        out.drawn << at;
    }
    if (!closed) out.drawn << onLine(end, cs.last(), ds.last());
    // Not a polyline after all: a curve a circle fits as well, or points
    // far from it.
    QPolygonF used;
    for (int i = segs.first().a; i <= segs.last().b && i < n - (closed ? 1 : 0); ++i) used << p.at(i);
    double most = 0;
    const double miss = polylineMiss(used, out.drawn, closed, &most);
    if (runs >= 3 && circleMiss(used) <= 1.2 * miss) return std::nullopt;
    if (most > 3 * tolerance) return std::nullopt;

    // Each corner: its angle, and its rounding - how far the stroke passes
    // from where its runs meet, as a circle of some radius would.
    for (int k = closed ? 0 : 1; k < runs; ++k) {
        const int before = (k - 1 + runs) % runs;
        const QPointF at = out.drawn.at(k);   // (an open one's start is drawn[0], its corner k drawn[k])
        const double angle = 180 - turnOf(ds.at(before), ds.at(k));
        out.angles << angle;
        double nearest = std::numeric_limits<double>::infinity();
        // (Between the points each run was fitted to: round the corner.)
        int from = fits.at(before).last, to = fits.at(k).first;
        if (to < from) to += n - 1;   // (a closed one's first corner)
        for (int i = from; i <= to; ++i) nearest = std::min(nearest, norm(p.at(closed ? i % (n - 1) : std::min(i, n - 1)) - at));
        const double noise = std::max(fits.at(before).run.spread, fits.at(k).run.spread);
        double radius = 0;
        if (std::isfinite(nearest) && nearest > std::max(width, 2.5 * noise) && angle < 170) {
            const double half = angle / 2 / Degrees;
            radius = nearest / (1 / std::sin(half) - 1);
        }
        out.radii << radius;
    }

    // Snapped: each horizontal or vertical run to what is near it, else to a
    // round value; its ends likewise along it.
    const double reach = std::max(width, 0.005 * size);
    QList<QPointF> sc = cs, sd = ds;
    for (int k = 0; k < runs; ++k) {
        Polyline::Run& r = out.runs[k];
        if (r.way != Polyline::Way::Horizontal && r.way != Polyline::Way::Vertical) continue;
        const bool across = r.way == Polyline::Way::Horizontal;
        r.snappedAt = snapped(r.at, across ? snaps.ys : snaps.xs, std::max(reach, 2 * r.spread), std::max(r.spread, width / 2), snaps.finest, &r.snappedTo);
        r.snapped = true;
        sc[k] = across ? QPointF(cs.at(k).x(), r.snappedAt) : QPointF(r.snappedAt, cs.at(k).y());
    }
    const auto snappedEnd = [&](QPointF point, int k, QString* what) {
        QPointF q = onLine(point, sc.at(k), sd.at(k));
        const Polyline::Run& r = out.runs.at(k);
        const double endReach = std::max({2 * width, 0.02 * size, 2 * r.spread}), tol = std::max(width / 2, r.spread);
        if (r.way == Polyline::Way::Horizontal) q.setX(snapped(q.x(), snaps.xs, endReach, tol, snaps.finest, what));
        else if (r.way == Polyline::Way::Vertical) q.setY(snapped(q.y(), snaps.ys, endReach, tol, snaps.finest, what));
        if (what->startsWith(QLatin1String("a ")) && what->endsWith(QLatin1String(" grid"))) what->clear();
        return q;
    };
    QString whatStart, whatEnd;
    if (!closed) out.snapped << snappedEnd(start, 0, &whatStart);
    for (int k = closed ? 0 : 1; k < runs; ++k) {
        QPointF at;
        out.snapped << (corner(k, sc, sd, &at) ? at : out.drawn.at(out.snapped.size()));
    }
    if (!closed) {
        out.snapped << snappedEnd(end, runs - 1, &whatEnd);
        out.endsSnappedTo = {whatStart, whatEnd};
    }

    // Near symmetric? Its corners (an open one's ends apart) mirrored about
    // an upright or a level line.
    if (!closed && out.drawn.size() >= 4) {
        const QPolygonF inner = out.drawn.mid(1, out.drawn.size() - 2);
        const int m = int(inner.size());
        const double allowed = std::max(2 * width, 0.05 * size);
        for (const bool upright : {true, false}) {
            const auto along = [upright](QPointF q) { return upright ? q.x() : q.y(); };
            const auto other = [upright](QPointF q) { return upright ? q.y() : q.x(); };
            double axis = 0;
            for (int j = 0; j < m; ++j) axis += (along(inner.at(j)) + along(inner.at(m - 1 - j))) / 2;
            axis /= m;
            double within = 0;
            for (int j = 0; j < m; ++j)
                within = std::max(within, std::hypot(2 * axis - along(inner.at(j)) - along(inner.at(m - 1 - j)), other(inner.at(j)) - other(inner.at(m - 1 - j))));
            if (within <= allowed && (out.axis.isEmpty() || within < out.within)) {
                out.axis = upright ? QStringLiteral("x") : QStringLiteral("y");
                out.axisAt = axis;
                out.within = within;
                // (Its ends, where the pen happened to come down and lift,
                //  more strictly.)
                const QPointF a = out.drawn.first(), b = out.drawn.last();
                out.endsToo = std::hypot(2 * axis - along(a) - along(b), other(a) - other(b)) <= std::max({2 * width, 0.02 * size, 2 * within});
            }
        }
    }
    return out;
}

QJsonObject polylineJson(const Polyline& pl)
{
    // As drawn, to a tenth of the stroke's breadth or so; snapped, exactly.
    double spread = 0;
    for (const Polyline::Run& r : pl.runs) spread = std::max(spread, r.spread);
    const double drawnStep = std::pow(10.0, std::floor(std::log10(std::max(spread, 1e-3)))) / 1;
    const double step = std::clamp(drawnStep, 1e-3, 10.0);
    QJsonObject o;
    if (pl.closed) o.insert(QStringLiteral("closed"), true);
    QJsonArray runs;
    for (const Polyline::Run& r : pl.runs) {
        QJsonObject one{{QStringLiteral("way"), wayName(r.way)}};
        if (r.way == Polyline::Way::Horizontal || r.way == Polyline::Way::Vertical) {
            one.insert(QStringLiteral("at"), roundedTo(r.at, step));
            one.insert(QStringLiteral("spread"), roundedTo(r.spread, step));
            if (r.snapped) {
                one.insert(QStringLiteral("snaps to"), roundedTo(r.snappedAt, 1e-4));
                one.insert(QStringLiteral("snapped to"), r.snappedTo);
            }
        } else {
            one.insert(QStringLiteral("angle"), std::round(r.angle));
            one.insert(QStringLiteral("spread"), roundedTo(r.spread, step));
        }
        runs.append(one);
    }
    o.insert(QStringLiteral("runs"), runs);
    QJsonArray drawn, snapped;
    for (const QPointF& q : pl.drawn) drawn.append(pointJson(q, step));
    for (const QPointF& q : pl.snapped) snapped.append(pointJson(q, 1e-4));
    o.insert(QStringLiteral("corners"), drawn);
    o.insert(QStringLiteral("snapped corners"), snapped);
    QJsonArray angles, rounded;
    for (int k = 0; k < pl.angles.size(); ++k) {
        angles.append(std::round(pl.angles.at(k)));
        if (pl.radii.value(k) > 0)
            rounded.append(QJsonObject{{QStringLiteral("corner"), pointJson(pl.drawn.at(pl.closed ? k : k + 1), step)},
                                       {QStringLiteral("radius"), roundedTo(pl.radii.at(k), pl.radii.at(k) >= 10 ? 1.0 : std::max(step, 0.1))}});
    }
    if (!angles.isEmpty()) o.insert(QStringLiteral("corner angles"), angles);
    if (!rounded.isEmpty()) o.insert(QStringLiteral("rounded"), rounded);
    if (!pl.closed && pl.endsSnappedTo.size() == 2 && (!pl.endsSnappedTo.at(0).isEmpty() || !pl.endsSnappedTo.at(1).isEmpty()))
        o.insert(QStringLiteral("ends snapped to"), QJsonArray::fromStringList(pl.endsSnappedTo));
    if (!pl.axis.isEmpty())
        o.insert(QStringLiteral("symmetric"), QJsonObject{{QStringLiteral("about"), pl.axis},
                                                          {QStringLiteral("at"), roundedTo(pl.axisAt, step)},
                                                          {QStringLiteral("within"), roundedTo(pl.within, step)},
                                                          {QStringLiteral("ends too"), pl.endsToo}});
    return o;
}

QString polylineSaid(const QJsonObject& o)
{
    const QJsonArray runs = o.value(QStringLiteral("runs")).toArray();
    const QJsonArray corners = o.value(QStringLiteral("corners")).toArray();
    const bool closed = o.value(QStringLiteral("closed")).toBool();
    if (runs.isEmpty() || corners.size() < runs.size()) return QString();
    QList<QPointF> rounded;
    QList<double> radius;
    for (const QJsonValue& v : o.value(QStringLiteral("rounded")).toArray()) {
        rounded << pointOf(v.toObject().value(QStringLiteral("corner")));
        radius << v.toObject().value(QStringLiteral("radius")).toDouble();
    }
    QStringList said;
    for (int k = 0; k < runs.size(); ++k) {
        const QJsonObject r = runs.at(k).toObject();
        const QString way = r.value(QStringLiteral("way")).toString();
        // Its ends: an open one's from corner k to k + 1; a closed one's
        // from corner k to k + 1, round to the first.
        const QPointF a = pointOf(corners.at(k)), b = pointOf(corners.at(closed ? (k + 1) % corners.size() : k + 1));
        QString one;
        if (way == QLatin1String("horizontal") || way == QLatin1String("vertical")) {
            const bool across = way == QLatin1String("horizontal");
            one = QStringLiteral("%1 at %2 ≈ %3 (±%4").arg(way, across ? QStringLiteral("y") : QStringLiteral("x"),
                                                          numberSaid(r.value(QStringLiteral("at")).toDouble()),
                                                          numberSaid(r.value(QStringLiteral("spread")).toDouble()));
            if (r.contains(QStringLiteral("snaps to")))
                one += QStringLiteral(", snaps to %1: %2").arg(numberSaid(r.value(QStringLiteral("snaps to")).toDouble()),
                                                                r.value(QStringLiteral("snapped to")).toString());
            one += QLatin1Char(')');
            if (across)
                one += k == 0 ? QStringLiteral(" from x = %1 to %2").arg(numberSaid(a.x()), numberSaid(b.x())) : QStringLiteral(" to x = %1").arg(numberSaid(b.x()));
            else
                one += QStringLiteral(" %1 y = %2").arg(b.y() >= a.y() ? QStringLiteral("up to") : QStringLiteral("down to"), numberSaid(b.y()));
            if (k == 0 && !across) one.replace(QStringLiteral(" up to"), QStringLiteral(" from y = %1 up to").arg(numberSaid(a.y())))
                                       .replace(QStringLiteral(" down to"), QStringLiteral(" from y = %1 down to").arg(numberSaid(a.y())));
        } else {
            one = QStringLiteral("%1 at %2°%3 to %4")
                      .arg(way, numberSaid(r.value(QStringLiteral("angle")).toDouble()),
                           k == 0 ? QStringLiteral(" from %1").arg(pointSaid(a)) : QString(), pointSaid(b));
        }
        for (int j = 0; j < rounded.size(); ++j)
            if (QLineF(rounded.at(j), b).length() < 1e-9 && (closed || k + 1 < runs.size()))
                one += QStringLiteral(", the corner there rounded (radius ≈ %1)").arg(numberSaid(radius.at(j)));
        said << one;
    }
    QString s = QStringLiteral("read as %1 polyline of %2 run(s): %3")
                    .arg(closed ? QStringLiteral("a closed") : QStringLiteral("a"))
                    .arg(runs.size())
                    .arg(said.join(QStringLiteral("; ")));
    if (o.contains(QStringLiteral("symmetric"))) {
        const QJsonObject m = o.value(QStringLiteral("symmetric")).toObject();
        s += QStringLiteral("; its corners symmetric about %1 ≈ %2 within %3 µm%4")
                 .arg(m.value(QStringLiteral("about")).toString(), numberSaid(m.value(QStringLiteral("at")).toDouble()),
                      numberSaid(m.value(QStringLiteral("within")).toDouble()),
                      closed ? QString() : m.value(QStringLiteral("ends too")).toBool() ? QStringLiteral(", its ends too") : QStringLiteral(", its ends not"));
    }
    QStringList snapped;
    for (const QJsonValue& v : o.value(QStringLiteral("snapped corners")).toArray()) snapped << pointSaid(pointOf(v));
    if (!snapped.isEmpty()) {
        s += QStringLiteral("; snapped, %1 %2").arg(closed ? QStringLiteral("its corners") : QStringLiteral("its points"), snapped.join(QLatin1Char(' ')));
        const QJsonArray ends = o.value(QStringLiteral("ends snapped to")).toArray();
        QStringList endsSaid;
        if (!ends.at(0).toString().isEmpty()) endsSaid << QStringLiteral("its start to %1").arg(ends.at(0).toString());
        if (!ends.at(1).toString().isEmpty()) endsSaid << QStringLiteral("its end to %1").arg(ends.at(1).toString());
        if (!endsSaid.isEmpty()) s += QStringLiteral(" (%1)").arg(endsSaid.join(QStringLiteral(", ")));
    }
    return s;
}

} // namespace qucs_s::markup
