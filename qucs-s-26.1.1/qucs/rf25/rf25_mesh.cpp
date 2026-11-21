/*
 * rf25_mesh.cpp - the conductors divided into cells, and what they cost
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "rf25_mesh.h"
#include "rf25_inductance.h"

#include "multiphysics/fem_expr.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <vector>
#include <array>
#include <cmath>
#include <limits>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

/// How many unknowns this build will set up a dense matrix for. Beyond it
/// the matrix alone is more memory than a desktop has, and the proposal
/// asks for a refusal rather than a machine that fills its swap.
constexpr int kMostUnknowns = 60000;
/// And held compressed: the full-wave solve's own bound.
constexpr int kMostCompressedUnknowns = 2000000;

// (The speed of light rf25_green.h's kLightSpeed.)

QJsonValue propertyOf(const Node& node, const QString& key)
{
    if (node.props.contains(key)) return node.props.value(key);
    if (const NodeKind* k = nodeKind(node.type))
        if (const PropertyDef* p = k->property(key)) return p->defaultValue;
    return {};
}

QString textOf(const Node& node, const QString& key)
{
    const QJsonValue v = propertyOf(node, key);
    return v.isString() ? v.toString() : v.isDouble() ? QString::number(v.toDouble()) : QString();
}

fem::Scope scopeOf(const QList<fem::Model::Parameter>& parameters)
{
    fem::Scope scope(&fem::Scope::builtins());
    for (const fem::Model::Parameter& p : parameters) {
        const fem::Expression e = fem::Expression::compile(p.expression, scope);
        if (e.isValid() && e.isConstant()) scope.setConstant(p.name, e.constant(), e.dim());
    }
    return scope;
}

double numberIn(const Node& node, const QString& key, const fem::Scope& scope, double fallback,
                const QString& where, QStringList* problems)
{
    const QString text = textOf(node, key);
    if (text.trimmed().isEmpty()) return fallback;
    const fem::Expression e = fem::Expression::compile(text, scope);
    if (!e.isValid() || !e.isConstant() || !std::isfinite(e.constant())) {
        if (problems != nullptr)
            *problems << tr("%1: %2 is not a number (%3).")
                             .arg(where, key,
                                  !e.isValid()      ? e.error()
                                  : !e.isConstant() ? tr("it is not a constant")
                                                    : tr("it comes to %1, no finite number").arg(e.constant()));
        return fallback;
    }
    return e.constant();
}

/// Where a side of \a length from \a from is cut: spaced at most \a step
/// apart, evenly, and the first space halved where \a gradeLow says, the
/// last where \a gradeHigh does. A side shorter than a step is one space
/// (two where graded).
/// How fine a conductor's outermost cells are made where it faces another
/// across a gap: no wider than this share of the gap (meshOf()).
/// A metal's thickness, as a share of the narrowest gap beside it, from
/// which the side walls' missing capacitance is said (meshOf()).
constexpr double kThickBesideGap = 0.25;
/// A metal solved as its two faces whose thickness is under this share of
/// its narrowest conductor's width is solved as a sheet (thickened()).
constexpr double kFacesApart = 0.02;
/// A metal Solved as Automatic is solved as its two faces where it is at
/// least this much of the narrowest gap beside it thick (thickened()): a
/// sheet there reads Z0 some per cents high - 2 µm of gold in 20 µm gaps,
/// a tenth, 4.8 % (Palace: 73.72 Ohm thin, 70.3 at 2 µm) - against B2's
/// quarter, where its capacitance across the gap goes visibly wrong.
constexpr double kAutomaticFaces = 1.0 / 40;
/// A conductor over or under another metal by half itself or more, this
/// much of its own height over the ground apart or less, is a plate of it:
/// its charge the other's, and its thickness over the ground not said.
constexpr double kPlateGap = 0.1;
/// A slanted outline's corners nearer than this share of its width (twice
/// its area over its perimeter) across the cuts between its parts are put
/// on one cut (piecesOf()): no part is thinner than that.
constexpr double kCornersMerged = 0.05;

/// Lines of two pieces that meet nearer than this share of the finest space
/// of any piece's own grid are one line (shareLines(), meshOf()).
constexpr double kMergeShare = 0.25;

QList<double> gridLines(double from, double length, double step, bool gradeLow, bool gradeHigh, double finestLow = 0,
                        double finestHigh = 0)
{
    QList<double> at;
    const int n = step > 0 ? std::max(1, int(std::ceil(length / step - 1e-9))) : 1;
    const double d = length / n;
    at << from;
    // A graded end's outermost space halved, and halved again while it is
    // wider than the finest asked there (a narrow gap it faces).
    if (gradeLow) {
        at << from + d / 2;
        for (double w = d / 2; finestLow > 0 && w > finestLow && w > 1e-6 * length; w /= 2) at << from + w / 2;
    }
    for (int i = 1; i < n; ++i) at << from + i * d;
    if (gradeHigh) {
        at << from + length - d / 2;
        for (double w = d / 2; finestHigh > 0 && w > finestHigh && w > 1e-6 * length; w /= 2) at << from + length - w / 2;
    }
    at << from + length;
    std::sort(at.begin(), at.end());
    // (One space graded at both ends is cut once, in its middle.)
    QList<double> out;
    for (const double x : std::as_const(at))
        if (out.isEmpty() || x - out.last() > 1e-9 * std::max(1.0, std::abs(length))) out << x;
    if (out.size() < 2) out = {from, from + length};
    return out;
}

/// \a outline cut into rectangles, where every side of it runs along x or
/// y: slabs between its corners' heights - across y, or across x, whichever
/// gives fewer, the broader where both give as many - each slab's spans
/// inside the outline, a span carried on into the next slab where that has
/// it too. So a tee is its line and its stub, an L its two arms, a bend its
/// legs. Empty where a side is slanted.
QList<QRectF> rectanglesOf(const QPolygonF& outline)
{
    QList<QPointF> points(outline.begin(), outline.end());
    if (points.size() > 1 && points.first() == points.last()) points.removeLast();
    const int m = int(points.size());
    if (m < 4) return {};
    const QRectF bounds = outline.boundingRect();
    const double tol = 1e-9 * std::max(1.0, std::max(bounds.width(), bounds.height()));
    for (int i = 0; i < m; ++i) {
        const QPointF a = points.at(i), b = points.at((i + 1) % m);
        if (std::abs(b.x() - a.x()) > tol && std::abs(b.y() - a.y()) > tol) return {};
    }
    const auto slabs = [&points, m, tol](bool acrossY) {
        QList<double> cuts;
        for (const QPointF& v : std::as_const(points)) cuts << (acrossY ? v.y() : v.x());
        std::sort(cuts.begin(), cuts.end());
        QList<double> unique;
        for (const double c : std::as_const(cuts))
            if (unique.isEmpty() || c - unique.last() > tol) unique << c;
        QList<QRectF> done, open;
        for (int s = 0; s + 1 < unique.size(); ++s) {
            const double c0 = unique.at(s), c1 = unique.at(s + 1), mid = (c0 + c1) / 2;
            // Where the outline's sides across the slab cross its middle:
            // inside between each odd crossing and the next.
            QList<double> crossings;
            for (int i = 0; i < m; ++i) {
                const QPointF a = points.at(i), b = points.at((i + 1) % m);
                const double a0 = acrossY ? a.y() : a.x(), b0 = acrossY ? b.y() : b.x();
                if (std::abs(b0 - a0) <= tol) continue;
                if (mid > std::min(a0, b0) && mid < std::max(a0, b0)) crossings << (acrossY ? a.x() : a.y());
            }
            std::sort(crossings.begin(), crossings.end());
            QList<QRectF> next;
            for (int k = 0; k + 1 < crossings.size(); k += 2) {
                const double u0 = crossings.at(k), u1 = crossings.at(k + 1);
                if (u1 - u0 <= tol) continue;
                int carried = -1;
                for (int o = 0; o < open.size() && carried < 0; ++o) {
                    const QRectF& r = open.at(o);
                    const double r0 = acrossY ? r.left() : r.top(), r1 = acrossY ? r.right() : r.bottom();
                    if (std::abs(r0 - u0) <= tol && std::abs(r1 - u1) <= tol) carried = o;
                }
                if (carried >= 0) {
                    QRectF r = open.takeAt(carried);
                    if (acrossY) r.setBottom(c1);
                    else r.setRight(c1);
                    next << r;
                } else {
                    next << (acrossY ? QRectF(QPointF(u0, c0), QPointF(u1, c1)) : QRectF(QPointF(c0, u0), QPointF(c1, u1)));
                }
            }
            done << open;
            open = next;
        }
        done << open;
        return done;
    };
    const QList<QRectF> byY = slabs(true), byX = slabs(false);
    const auto narrowest = [](const QList<QRectF>& rs) {
        double n = std::numeric_limits<double>::infinity();
        for (const QRectF& r : rs) n = std::min(n, std::min(r.width(), r.height()));
        return n;
    };
    const QList<QRectF>& chosen = byY.size() < byX.size() || (byY.size() == byX.size() && narrowest(byY) >= narrowest(byX))
                                      ? byY
                                      : byX;
    // (Their areas add up to the outline's, or it was no simple outline.)
    double sum = 0;
    for (const QRectF& r : chosen) sum += r.width() * r.height();
    if (std::abs(sum - areaOf(outline)) > 1e-6 * std::max(1e-30, areaOf(outline))) return {};
    return chosen;
}

/// A piece of a conductor cut between its corners (piecesOf()): a
/// rectangle, or a trapezoid along a slanted side.
struct Slab {
    QPolygonF outline;
    QRectF box;
    bool rectangle = false;
    /// How wide it is: a rectangle's short side; a trapezoid's mean width
    /// across its slab, or the slab's own depth where that is less. Where
    /// neither of its two sides along the slab is the outline's - both cuts
    /// between slabs -, its width across the slab alone (a trapezoid's at
    /// right angles to its slanted sides), and \a along the way across the
    /// slabs: its depth is no width of the conductor.
    double feature = 0;
    /// The way its length runs where \a feature says so: 0 along x, 1 along
    /// y; -1: as its box has it.
    int along = -1;
};

/*!
 * \a outline - a side of it slanted (rectanglesOf() takes it where none
 * is) - cut into slabs between its corners' coordinates, across x or across
 * y: the way its slanted sides run least along the cuts - so that each is
 * in a trapezoid as thin as it can be - then the way of fewer pieces. Each
 * slab's spans inside the outline are a rectangle where both its sides run
 * straight across the slab, carried on into the next slab where that has
 * the same span, and a trapezoid else. So a line drawn inside a pad with
 * tapers is that line, a rectangle, with its pads' rectangles and its
 * tapers' trapezoids beside it, and its grounds alike; meshed as each
 * piece's own width, joined edge to edge, the line's cross-section the line
 * drawn alone's. Sized by the conductor's 2 area / perimeter whole, the line
 * inside the polygon was meshed unlike the line drawn alone - its eps_eff
 * 0.6-0.8 % over it -, and a filter's 0.7 mm line in a polygon called 2694
 * µm wide had one or two cells across it. Empty where the pieces' areas do not add up to the outline's
 * (it crosses itself, or is no simple outline).
 */
QList<Slab> piecesOf(const QPolygonF& outline, double merge = 0, double* moved = nullptr, double across = 0)
{
    if (moved != nullptr) *moved = 0;
    QList<QPointF> given(outline.begin(), outline.end());
    if (given.size() > 1 && given.first() == given.last()) given.removeLast();
    const int m = int(given.size());
    if (m < 3) return {};
    const QRectF bounds = outline.boundingRect();
    const double tol = 1e-9 * std::max(1.0, std::max(bounds.width(), bounds.height()));
    struct Cut {
        QList<Slab> slabs;
        double area = 0;    // the outline's, its corners snapped as the cuts have them
        double moved = 0;   // the furthest the outline moved off itself
    };
    // A keyhole's slit - an edge run back along by another, there and back,
    // the way a polygon holds a hole (a ring of the model's own, a file's
    // polygon with a hole) -: no side, so left out where the slabs cross the
    // outline. Crossed as two sides, it cut every slab it crossed into the
    // piece on one side and the piece on the other, each meshed on its own,
    // and their cells met along a third of it: the current across the ring
    // there through a third of its width (the review of 747a2cfd).
    QSet<int> slit;
    {
        // (Each edge by its two ends, rounded to the tolerance: one met the
        //  other way round is the slit's other side.)
        const auto keyOf = [tol](QPointF a, QPointF b) {
            return QStringLiteral("%1,%2,%3,%4")
                .arg(std::llround(a.x() / tol))
                .arg(std::llround(a.y() / tol))
                .arg(std::llround(b.x() / tol))
                .arg(std::llround(b.y() / tol));
        };
        QHash<QString, int> edges;
        for (int i = 0; i < m; ++i) {
            const QPointF a = given.at(i), b = given.at((i + 1) % m);
            if (std::hypot(b.x() - a.x(), b.y() - a.y()) <= tol) continue;
            const auto back = edges.constFind(keyOf(b, a));
            if (back != edges.cend()) {
                slit << i << back.value();
                continue;
            }
            edges.insert(keyOf(a, b), i);
        }
    }
    const auto cut = [&given, &slit, m, tol, merge, across](bool acrossY) {
        // (u: along the slab; c: across it, the corners' coordinates.)
        const auto cOf = [acrossY](QPointF p) { return acrossY ? p.y() : p.x(); };
        const auto uOf = [acrossY](QPointF p) { return acrossY ? p.x() : p.y(); };
        // The corners' coordinates across the cuts snapped into levels at
        // least \a merge apart, each level where its lowest corner is, so
        // that no corner moves more than \a merge: a curve's two sides have
        // their corners at heights of their own, and a part between an
        // inner corner and an outer one a hair higher was a sliver, meshed
        // by its own depth - a circle of 180 segments a turn, its parts
        // 0.0098 µm deep, 170,381 cells for a strip of 1,335 by 8 µm. Moved across the cuts alone, along
        // a side near the slabs' way it moves little of the area.
        //
        // Each moved along the steeper of its two sides down to its
        // neighbour, so that it stays on that side's line - the outline's
        // gaps and widths as they were along it -, or straight across where
        // that side does not reach the level: moved straight across, a
        // dodecagonal spiral's corners narrowed its 3 µm gaps to 2.85 where
        // its sides run near the cuts' way. And a curve's corners alone, those
        // turning under 20 degrees: a polygon's - a dodecagon's turn 30 - are
        // its own, its gaps between them as drawn.
        //
        // Straight across \a across at most - the curves' tolerance -, else
        // not moved: near a curve's cap, where neither side reaches the
        // level, a corner dropped by up to \a merge flattened the cap into a
        // step, a disc of 100 µm 4.9 µm off its outline and a plane's round
        // opening 15 µm (the review of 2c232022). And how far the outline
        // moved is said as how far it is off the drawn one, not how far a
        // corner slid along its side: on the 8 µm spiral they slid 6.4 µm
        // where its outline moved 0.37 (the same review).
        Cut out;
        QList<int> moving;
        const auto offSegment = [](QPointF p, QPointF a, QPointF b) {
            const QPointF d = b - a;
            const double len2 = d.x() * d.x() + d.y() * d.y();
            const double t = len2 > 0 ? std::clamp(((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / len2, 0.0, 1.0) : 0.0;
            return std::hypot(p.x() - a.x() - t * d.x(), p.y() - a.y() - t * d.y());
        };
        // (How far a corner of \a i moved to \a to takes the outline off
        //  itself, its neighbours as drawn: the corner from the drawn sides,
        //  the drawn corner from the sides it now makes.)
        const auto offBy = [&](int i, QPointF to) {
            const QPointF a = given.at((i + m - 1) % m), at = given.at(i), b = given.at((i + 1) % m);
            return std::max(std::min(offSegment(to, a, at), offSegment(to, at, b)), std::min(offSegment(at, a, to), offSegment(at, to, b)));
        };
        QList<QPointF> points = given;
        if (merge > 0) {
            QList<std::pair<double, int>> order;
            for (int i = 0; i < m; ++i) order.push_back({cOf(given.at(i)), i});
            std::sort(order.begin(), order.end());
            double level = order.first().first;
            for (const auto& [c, i] : std::as_const(order)) {
                if (c - level > merge) level = c;
                if (c == level) continue;
                const QPointF at = given.at(i);
                // (A curve's corners alone, turning under 20 degrees: a
                //  polygon's corners are its own, and stay.)
                {
                    const QPointF a = at - given.at((i + m - 1) % m), b = given.at((i + 1) % m) - at;
                    const double la = std::hypot(a.x(), a.y()), lb = std::hypot(b.x(), b.y());
                    if (!(la > 0) || !(lb > 0) || (a.x() * b.x() + a.y() * b.y()) / (la * lb) < std::cos(20 * M_PI / 180)) continue;
                }
                QPointF to = acrossY ? QPointF(at.x(), level) : QPointF(level, at.y());
                if (c - level > across) to = at;   // (straight across too far: it stays unless a side slides it)
                double steepest = 0;
                for (const int j : {(i + m - 1) % m, (i + 1) % m}) {
                    const QPointF next = given.at(j);
                    const double drop = c - cOf(next);
                    if (!(drop >= c - level)) continue;   // (it does not reach the level)
                    const double steep = drop / std::max(tol, std::hypot(next.x() - at.x(), next.y() - at.y()));
                    if (steep <= steepest) continue;
                    // (A slide far along a side tilts the other by its turn:
                    //  the curves' tolerance off the outline at most, as a
                    //  drop straight across. Half the merge - a twentieth
                    //  of the whole shape's width - took a disc of 100 µm
                    //  drawn of 36 segments 1.5 µm off, of 1 mm 15 µm (the
                    //  review of 10b60005), and the 24 segments a turn
                    //  spiral's 15-degree corners 0.56 µm off its 8 µm.)
                    const QPointF slid = at + (next - at) * ((c - level) / drop);
                    if (offBy(i, slid) > across) continue;
                    steepest = steep;
                    to = slid;
                }
                if (to == at) continue;
                points[i] = to;
                moving << i;
            }
            // (How far off the drawn outline: each corner moved from the
            //  sides it now has, and the drawn corner from the sides it now
            //  makes, the furthest of either.)
            for (const int i : std::as_const(moving)) {
                const int a = (i + m - 1) % m, b = (i + 1) % m;
                const double newOff = std::min(offSegment(points.at(i), given.at(a), given.at(i)), offSegment(points.at(i), given.at(i), given.at(b)));
                const double oldOff = std::min(offSegment(given.at(i), points.at(a), points.at(i)), offSegment(given.at(i), points.at(i), points.at(b)));
                out.moved = std::max({out.moved, newOff, oldOff});
            }
        }
        const QPolygonF whole(QList<QPointF>(points.begin(), points.end()));
        out.area = areaOf(whole);
        QList<double> cuts;
        for (const QPointF& v : std::as_const(points)) cuts << cOf(v);
        std::sort(cuts.begin(), cuts.end());
        QList<double> unique;
        for (const double c : std::as_const(cuts))
            if (unique.isEmpty() || c - unique.last() > tol) unique << c;
        struct Span {
            double u0[2], u1[2];   // its two sides at its low and its high c
            double c0, c1;
            int e0 = -1, e1 = -1;   // the outline's edges its two sides lie on
        };
        QList<Slab> done;
        QList<Span> open;
        const auto finish = [&done, &whole, acrossY, tol](const Span& sp) {
            Slab slab;
            const auto at = [acrossY](double u, double c) { return acrossY ? QPointF(u, c) : QPointF(c, u); };
            slab.outline = QPolygonF({at(sp.u0[0], sp.c0), at(sp.u1[0], sp.c0), at(sp.u1[1], sp.c1), at(sp.u0[1], sp.c1)});
            slab.box = slab.outline.boundingRect();
            slab.rectangle = std::abs(sp.u0[0] - sp.u0[1]) <= tol && std::abs(sp.u1[0] - sp.u1[1]) <= tol;
            const double depth = sp.c1 - sp.c0, wide = 0.5 * ((sp.u1[0] - sp.u0[0]) + (sp.u1[1] - sp.u0[1]));
            slab.feature = std::min(depth, wide);
            // Its two sides along the slab: the outline's where just beyond the
            // middle of either is outside it. Both cuts between slabs, its depth
            // is only how far apart two corners of the outline are - a
            // dodecagonal spiral's turns have corners 0.012 µm apart in height,
            // and the slab between them was meshed as a conductor 0.012 µm wide,
            // 12,770 cells for a spiral of 6,500 - so it goes by its width across the slab, its length across
            // the slabs.
            const double e = 1e3 * tol;
            const bool lowCut = whole.containsPoint(at(0.5 * (sp.u0[0] + sp.u1[0]), sp.c0 - e), Qt::OddEvenFill);
            const bool highCut = whole.containsPoint(at(0.5 * (sp.u0[1] + sp.u1[1]), sp.c1 + e), Qt::OddEvenFill);
            if (lowCut && highCut) {
                const double sideA = std::hypot(sp.u0[1] - sp.u0[0], depth), sideB = std::hypot(sp.u1[1] - sp.u1[0], depth);
                slab.feature = slab.rectangle ? wide : std::min(wide, wide * depth / std::max(sideA, sideB));
                slab.along = acrossY ? 1 : 0;
            }
            done << slab;
        };
        for (int s = 0; s + 1 < unique.size(); ++s) {
            const double c0 = unique.at(s), c1 = unique.at(s + 1), mid = (c0 + c1) / 2;
            QList<std::array<double, 4>> crossings;   // (u at mid, at c0, at c1; the edge)
            for (int i = 0; i < m; ++i) {
                if (slit.contains(i)) continue;
                const QPointF a = points.at(i), b = points.at((i + 1) % m);
                const double ca = cOf(a), cb = cOf(b);
                if (std::abs(cb - ca) <= tol || mid <= std::min(ca, cb) || mid >= std::max(ca, cb)) continue;
                const auto uAt = [&](double c) { return uOf(a) + (uOf(b) - uOf(a)) * (c - ca) / (cb - ca); };
                crossings.push_back({uAt(mid), uAt(c0), uAt(c1), double(i)});
            }
            std::sort(crossings.begin(), crossings.end(), [](const auto& x, const auto& y) { return x[0] < y[0]; });
            QList<Span> next;
            for (int k = 0; k + 1 < crossings.size(); k += 2) {
                const auto& lo = crossings.at(k);
                const auto& hi = crossings.at(k + 1);
                if (hi[0] - lo[0] <= tol) continue;
                Span sp{{lo[1], lo[2]}, {hi[1], hi[2]}, c0, c1, int(lo[3]), int(hi[3])};
                const bool straight = std::abs(lo[1] - lo[2]) <= tol && std::abs(hi[1] - hi[2]) <= tol;
                // Carried on where the span before it went on straight, or
                // where both its sides lie on the same edges of the outline
                // as that one's: a slanted side's trapezoid is one from
                // corner to corner of its own, not cut again at the height of
                // every corner elsewhere. Cut so, an octagonal spiral's turns
                // were 221 slivers along their slanted sides, each meshed by
                // its own width - 0.83 µm of a turn 18 wide -, ten times the
                // cells of a square spiral of a longer turn.
                int carried = -1;
                for (int o = 0; o < open.size() && carried < 0; ++o) {
                    const Span& was = open.at(o);
                    if (std::abs(was.c1 - c0) > tol || std::abs(was.u0[1] - lo[1]) > tol || std::abs(was.u1[1] - hi[1]) > tol)
                        continue;
                    const bool wasStraight = std::abs(was.u0[0] - was.u0[1]) <= tol && std::abs(was.u1[0] - was.u1[1]) <= tol;
                    if ((straight && wasStraight) || (was.e0 == sp.e0 && was.e1 == sp.e1)) carried = o;
                }
                if (carried >= 0) {
                    sp = open.takeAt(carried);
                    sp.c1 = c1;
                    sp.u0[1] = lo[2];
                    sp.u1[1] = hi[2];
                }
                next << sp;
            }
            for (const Span& sp : std::as_const(open)) finish(sp);
            open = next;
        }
        for (const Span& sp : std::as_const(open)) finish(sp);
        out.slabs = done;
        return out;
    };
    // How far the slanted sides run along the cuts of each way: across y,
    // their extent in x; across x, in y.
    double runX = 0, runY = 0;
    for (int i = 0; i < m; ++i) {
        const QPointF a = given.at(i), b = given.at((i + 1) % m);
        if (std::abs(a.x() - b.x()) <= tol || std::abs(a.y() - b.y()) <= tol) continue;
        runX += std::abs(a.x() - b.x());
        runY += std::abs(a.y() - b.y());
    }
    const Cut byY = cut(true), byX = cut(false);
    const bool acrossY = runX < runY - tol || (std::abs(runX - runY) <= tol && byY.slabs.size() <= byX.slabs.size());
    const Cut& chosen = acrossY ? byY : byX;
    double sum = 0;
    for (const Slab& p : chosen.slabs) sum += areaOf(p.outline);
    if (chosen.slabs.isEmpty() || std::abs(sum - chosen.area) > 1e-6 * std::max(1e-30, chosen.area)) return {};
    if (moved != nullptr) *moved = chosen.moved;
    return chosen.slabs;
}

/// The centroid of \a outline, whose area is \a area (> 0).
QPointF centroidOf(const QPolygonF& outline, double area)
{
    double cx = 0, cy = 0, twice = 0;
    const int n = int(outline.size());
    for (int i = 0; i < n; ++i) {
        const QPointF p = outline.at(i), q = outline.at((i + 1) % n);
        const double cross = p.x() * q.y() - q.x() * p.y();
        twice += cross;
        cx += (p.x() + q.x()) * cross;
        cy += (p.y() + q.y()) * cross;
    }
    if (!(std::abs(twice) > 1e-12 * area)) return outline.boundingRect().center();
    return QPointF(cx / (3 * twice), cy / (3 * twice));
}

/// A strip's quadrilaterals along its centreline (Conductor::spine): each
/// segment's left and right corners at its two ends, mitred as
/// stripOutline() makes its outline. Empty where a segment's side runs back
/// against its centreline - a turn sharp enough to fold the mitre over -,
/// which is then meshed as any other polygon.
struct StripQuads {
    QList<QPointF> left, right;   ///< at each corner of the centreline
    QList<double> lengths;        ///< each segment's along the centreline
    QList<double> angles;         ///< each segment's way, radians from x
    bool ok() const { return lengths.size() >= 1; }
};

StripQuads stripQuads(const QPolygonF& spine, double width)
{
    StripQuads out;
    const int n = int(spine.size());
    if (n < 2 || !(width > 0)) return out;
    const QPolygonF outline = stripOutline(spine, width);
    if (outline.size() != 2 * n) return out;
    for (int i = 0; i < n; ++i) {
        out.left << outline.at(i);
        out.right << outline.at(2 * n - 1 - i);
    }
    QList<double> lengths, angles;
    for (int k = 0; k + 1 < n; ++k) {
        const QPointF d = spine.at(k + 1) - spine.at(k);
        const double l = std::hypot(d.x(), d.y());
        if (!(l > 0)) return {};
        const QPointF dl = out.left.at(k + 1) - out.left.at(k), dr = out.right.at(k + 1) - out.right.at(k);
        if (!(dl.x() * d.x() + dl.y() * d.y() > 0) || !(dr.x() * d.x() + dr.y() * d.y() > 0)) return {};
        lengths << l;
        angles << std::atan2(d.y(), d.x());
    }
    out.lengths = lengths;
    out.angles = angles;
    return out;
}

/*!
 * The cells of a strip (Conductor::spine) cut along its centreline: across
 * its width \a perFeature cells - their outermost spaces halved where
 * \a edges, as a rectangle's are - and along its whole length cells
 * \a stretch times as long as they are wide (up to \a size), its two ends'
 * outermost spaces halved too: as many as its length holds nearest, a single
 * segment the rectangle's own count. Where a cell spans a corner of the
 * centreline, its sides bend there, through the mitre's points at the same
 * fractions across - so that a strip of many short segments takes the cells
 * its length does, a turned rectangle the rectangle's, and the cells tile
 * its outline exactly, each meeting the next edge to edge. Each cell is in
 * the frame of the chord its middle runs along (MeshCell::angle). \a across
 * is the cells' width across it and \a isStrip whether it is long enough to
 * stretch.
 */
QList<MeshCell> stripCells(const Conductor& conductor, const StripQuads& quads, double size, int perFeature, double stretch,
                           bool edges, double* across, bool* isStrip)
{
    QList<MeshCell> out;
    const double width = conductor.spineWidth;
    const int segments = int(quads.lengths.size());
    QList<double> starts{0};   // (each corner's distance along the centreline)
    for (const double l : quads.lengths) starts << starts.last() + l;
    const double total = starts.last();
    double step = size;
    if (perFeature > 0) step = std::min(step, width / perFeature);
    const bool strip = total >= 3 * width;
    const double along = strip && stretch > 1 ? std::min(size, stretch * step) : step;
    *across = step;
    *isStrip = strip;
    QList<double> vs = gridLines(0, width, step, edges, edges);
    for (double& v : vs) v /= width;
    vs.last() = 1;
    const int count = segments == 1 ? std::max(1, int(std::ceil(total / along - 1e-9))) : std::max(1, int(std::lround(total / along)));
    QList<double> ss = gridLines(0, total, total / count, edges, edges);
    ss.first() = 0;
    ss.last() = total;
    const double hair = 1e-9 * std::max(1.0, total);
    // A point at \a at along the centreline, \a v of the way across from its
    // left; at a corner, the mitre's. \a k: the segment it is on.
    const auto segmentOf = [&](double at) {
        int k = 0;
        while (k + 1 < segments && at > starts.at(k + 1)) ++k;
        return k;
    };
    const auto point = [&](double at, double v) {
        const int k = segmentOf(at);
        const double u = at <= starts.at(k) ? 0 : at >= starts.at(k + 1) ? 1 : (at - starts.at(k)) / quads.lengths.at(k);
        const QPointF l0 = quads.left.at(k), l1 = quads.left.at(k + 1), r0 = quads.right.at(k), r1 = quads.right.at(k + 1);
        const QPointF a = u == 0 ? l0 : u == 1 ? l1 : l0 * (1 - u) + l1 * u;
        const QPointF b = u == 0 ? r0 : u == 1 ? r1 : r0 * (1 - u) + r1 * u;
        return v == 0 ? a : v == 1 ? b : a * (1 - v) + b * v;
    };
    const auto corner = [&](int k, double v) {
        const QPointF a = quads.left.at(k), b = quads.right.at(k);
        return v == 0 ? a : v == 1 ? b : a * (1 - v) + b * v;
    };
    for (int i = 0; i + 1 < ss.size(); ++i) {
        const double s0 = ss.at(i), s1 = ss.at(i + 1);
        QList<int> inside;   // (the centreline's corners strictly within it)
        for (int k = 1; k < segments; ++k)
            if (starts.at(k) > s0 + hair && starts.at(k) < s1 - hair) inside << k;
        const QPointF c0 = (point(s0, 0) + point(s0, 1)) / 2, c1 = (point(s1, 0) + point(s1, 1)) / 2;
        const QPointF chord = c1 - c0;
        const double angle = std::atan2(chord.y(), chord.x()), middle = (s0 + s1) / 2;
        for (int j = 0; j + 1 < vs.size(); ++j) {
            const double v0 = vs.at(j), v1 = vs.at(j + 1);
            QPolygonF outline;
            outline << point(s0, v0);
            for (const int k : std::as_const(inside)) outline << corner(k, v0);
            outline << point(s1, v0) << point(s1, v1);
            for (qsizetype m = inside.size() - 1; m >= 0; --m) outline << corner(inside.at(m), v1);
            outline << point(s0, v1);
            MeshCell cell;
            cell.layer = conductor.layer;
            cell.share = conductor.share;
            cell.outline = outline;
            cell.area = areaOf(outline);
            if (!(cell.area > 0)) continue;
            cell.centre = centroidOf(outline, cell.area);
            cell.onEdge = j == 0 || j + 2 == vs.size() || i == 0 || i + 2 == ss.size();
            // (Its frame's rectangle: as long as its middle's chord, as wide
            //  as it is across at its middle, scaled to its own area.)
            const double a = std::hypot(chord.x(), chord.y());
            const QPointF wide = point(middle, v1) - point(middle, v0);
            const double b = std::hypot(wide.x(), wide.y());
            const double scale = a > 0 && b > 0 ? std::sqrt(cell.area / (a * b)) : 1;
            cell.angle = angle;
            cell.frame = QSizeF(a * scale, b * scale);
            out << cell;
        }
    }
    return out;
}

/// How far apart outlines \a a and \a b are at their nearest, where they
/// do not cross: the least distance from a corner of either to a side of
/// the other.
double outlineDistance(const QPolygonF& a, const QPolygonF& b)
{
    double best = std::numeric_limits<double>::infinity();
    const auto toSides = [&best](const QPolygonF& points, const QPolygonF& sides) {
        const int n = int(sides.size());
        for (const QPointF& p : points)
            for (int i = 0; i < n; ++i) {
                const QPointF s = sides.at(i), e = sides.at((i + 1) % n), d = e - s;
                const double len2 = d.x() * d.x() + d.y() * d.y();
                const double t = len2 > 0 ? std::clamp(((p.x() - s.x()) * d.x() + (p.y() - s.y()) * d.y()) / len2, 0.0, 1.0) : 0.0;
                best = std::min(best, std::hypot(p.x() - s.x() - t * d.x(), p.y() - s.y() - t * d.y()));
            }
    };
    toSides(a, b);
    toSides(b, a);
    return best;
}

/// The lines of \a mine and of \a theirs within [\a lo, \a hi] each made
/// the other's too: where two pieces meet along that stretch, so that
/// every cell of one there shares a whole edge with a cell of the other.
/// Whether \a mine or \a theirs gained any - or lost one.
///
/// Two lines nearer than \a merge are one there, the lower kept - the
/// stretch's own ends before either -, and the other taken out of both:
/// each piece's grid graded from its own ends, the two series met in lines
/// a hair apart, and a spiral's cells came out half a nanometre wide
/// beside its turns' gradings ("Solved on
/// 4648 cell(s) from 0.000548 to 14.9 µm", and the loop shorted). The lower
/// of a cluster, and an end over any, is a choice both pieces make alike
/// whatever they held, so that rounds of sharing settle.
bool shareLines(QList<double>& mine, QList<double>& theirs, double lo, double hi, double tol, bool ends = true,
                double merge = 0)
{
    // (\a ends false: the lines strictly inside alone, not the stretch's
    //  ends - where a slanted side's part meets another, an end at a slanted
    //  corner is no line of either.)
    const double in = ends ? -tol : tol;
    QList<double> both;
    for (const double x : std::as_const(mine))
        if (x >= lo + in && x <= hi - in) both << x;
    for (const double x : std::as_const(theirs))
        if (x >= lo + in && x <= hi - in) both << x;
    if (ends) both << lo << hi;
    std::sort(both.begin(), both.end());
    bool gained = false;
    if (merge > tol) {
        // The lines that stay: the ends, then from the lowest up each one
        // further than \a merge from the last kept and from the next end.
        const auto isEnd = [&](double x) { return ends && (std::abs(x - lo) <= tol || std::abs(x - hi) <= tol); };
        QList<double> kept;
        for (const double x : std::as_const(both)) {
            if (!kept.isEmpty() && x - kept.last() <= tol) continue;
            if (isEnd(x)) {
                // (An end displaces the line kept just under it.)
                while (!kept.isEmpty() && !isEnd(kept.last()) && x - kept.last() <= merge) kept.removeLast();
                kept << x;
                continue;
            }
            if (!kept.isEmpty() && x - kept.last() <= merge) continue;
            if (ends && hi - x <= merge && hi - x > tol) continue;
            kept << x;
        }
        // Each side's lines inside the stretch that are not kept, out.
        for (QList<double>* lines : {&mine, &theirs}) {
            const qsizetype was = lines->size();
            lines->erase(std::remove_if(lines->begin(), lines->end(),
                                        [&](double x) {
                                            if (x < lo - tol || x > hi + tol) return false;
                                            if (std::abs(x - lo) <= tol || std::abs(x - hi) <= tol) return false;
                                            for (const double k : std::as_const(kept))
                                                if (std::abs(x - k) <= tol) return false;
                                            return true;
                                        }),
                         lines->end());
            gained = gained || lines->size() != was;
        }
        both = kept;
    }
    for (QList<double>* lines : {&mine, &theirs})
        for (const double x : std::as_const(both)) {
            bool known = false;
            for (const double y : std::as_const(*lines)) known = known || std::abs(x - y) <= tol;
            if (known) continue;
            lines->insert(std::lower_bound(lines->begin(), lines->end(), x), x);
            gained = true;
        }
    return gained;
}

/*!
 * \a cells made to meet side to side: into each side of a cell that runs
 * along x or y, the corners of the cells of its layer and its conductor
 * (\a conductor, by cell) that lie strictly inside it. A side of 0 to 10
 * facing two of 0 to 5 and 5 to 10 is two sides then, each one of theirs, so
 * that the current crosses it - paired by edgeKey(), a side met by cells of
 * other lines met none. The cells, their areas and centres stay as they are.
 *
 * Where pieces of a conductor meet, their lines are shared (shareLines()),
 * but not every one of them: at three cells across, a slanted tee's stem met
 * its bar along no side at all, its 20 µm base cut, and a chamfered pad's
 * trapezoid and the rectangle over it met along 70 µm of 100 (the review of
 * 9acb72fd). Shared further, the lines moved meshes that must stay as they
 * are - a line inside its pads meshed as drawn alone.
 */
void conform(QList<MeshCell>& cells, QList<int>& conductor)
{
    if (cells.isEmpty() || conductor.size() != cells.size()) return;
    // (A nanometre, as edgeKey() rounds: corners nearer than that are one,
    //  and sides of cells that near each other pair already. Finer, a
    //  taper's lines 0.3 nm off a line's were put in its cells' sides, and
    //  the slivers they made paired with nothing.)
    const double tol = 1e-3;
    // Each coordinate within a nanometre of the next taken as one - the
    // value most corners of the run have, so that a line's own grid stays as
    // it is beside a taper's lines 0.3 nm off it -: sides that are one then
    // have one middle. Rounded by edgeKey() as they were, the two sides of a
    // hexagon's cells at 3.333 and 3.334 µm fell either side of a nanometre
    // and paired with nothing.
    {
        QList<double> xs, ys;
        for (const MeshCell& c : std::as_const(cells))
            for (const QPointF& p : std::as_const(c.outline)) {
                xs << p.x();
                ys << p.y();
            }
        const auto runsOf = [tol](QList<double>& values) {
            std::sort(values.begin(), values.end());
            QList<double> taken(values.size());   // (each value's run's most common, at every value of it)
            for (int i = 0; i < values.size();) {
                // (A run by the gaps between its values, each a nanometre or
                //  less: from its first value, 0, 0.0007 and 0.0014 were two
                //  runs, and the corners 0.7 nm apart two lines - the review of
                //  68c22cd9.)
                int j = i + 1;
                while (j < values.size() && values.at(j) - values.at(j - 1) <= tol) ++j;
                double most = values.at(i);
                int count = 0;
                for (int k = i; k < j;) {
                    int l = k + 1;
                    while (l < j && values.at(l) == values.at(k)) ++l;
                    if (l - k > count) {
                        count = l - k;
                        most = values.at(k);
                    }
                    k = l;
                }
                for (int k = i; k < j; ++k) taken[k] = most;
                i = j;
            }
            return taken;
        };
        const QList<double> xFirst = runsOf(xs), yFirst = runsOf(ys);
        const auto snapped = [](const QList<double>& sorted, const QList<double>& firsts, double v) {
            const auto it = std::lower_bound(sorted.cbegin(), sorted.cend(), v);
            return it == sorted.cend() ? v : firsts.at(int(it - sorted.cbegin()));
        };
        for (MeshCell& c : cells)
            for (QPointF& p : c.outline) p = QPointF(snapped(xs, xFirst, p.x()), snapped(ys, yFirst, p.y()));
        // Each cell's corners once, and a cell narrower than a nanometre -
        // left with no width by the snap - taken out: its neighbours either
        // side then have its line in common, and meet. Kept, its two sides on
        // one line had one middle with theirs, three cells to a side that
        // the solve leaves out as none's, and a ring with feeds was in
        // pieces.
        QList<MeshCell> kept;
        QList<int> keptConductor;
        for (int i = 0; i < cells.size(); ++i) {
            MeshCell c = cells.at(i);
            QPolygonF o;
            for (const QPointF& p : std::as_const(c.outline))
                if (o.isEmpty() || p != o.last()) o << p;
            while (o.size() > 1 && o.first() == o.last()) o.removeLast();
            // (Its mean thickness, its area over its longer side, under half
            //  a nanometre: on one line, the shoelace's rounding leaves it an
            //  area of 1e-14 µm² and not nought.)
            const QRectF box = o.boundingRect();
            if (o.size() < 3 || !(std::abs(areaOf(o)) > 0.5 * tol * std::max(box.width(), box.height()))) continue;
            c.outline = o;
            kept << c;
            keptConductor << conductor.at(i);
        }
        cells = kept;
        conductor = keptConductor;
    }
    // The corners on each line along x (by y) and along y (by x), by layer and
    // conductor: snapped, a line's corners have its coordinate exactly, its
    // key. Each line's positions sorted once, a side's corners strictly
    // inside it found by halving: walked whole for each side, a mesh of
    // 91,204 cells took four times as long as made without (the review of
    // eba10a28).
    QHash<QString, int> layers;
    for (const MeshCell& c : std::as_const(cells))
        if (!layers.contains(c.layer)) layers.insert(c.layer, int(layers.size()));
    struct Line {
        int layer, conductor;
        bool alongX;
        double at;
        bool operator==(const Line& o) const
        {
            return layer == o.layer && conductor == o.conductor && alongX == o.alongX && at == o.at;
        }
    };
    struct LineHash {
        std::size_t operator()(const Line& l) const noexcept
        {
            return std::hash<double>()(l.at) ^ (std::size_t(l.layer) * 0x9e3779b97f4a7c15ULL) ^ (std::size_t(l.conductor) << 20)
                   ^ (l.alongX ? 0x5bd1e995ULL : 0);
        }
    };
    std::unordered_map<Line, std::vector<double>, LineHash> lines;
    for (int i = 0; i < cells.size(); ++i) {
        const int layer = layers.value(cells.at(i).layer);
        for (const QPointF& p : std::as_const(cells.at(i).outline)) {
            lines[Line{layer, conductor.at(i), true, p.y()}].push_back(p.x());
            lines[Line{layer, conductor.at(i), false, p.x()}].push_back(p.y());
        }
    }
    for (auto& [line, at] : lines) {
        std::sort(at.begin(), at.end());
        at.erase(std::unique(at.begin(), at.end()), at.end());
    }
    for (int index = 0; index < cells.size(); ++index) {
        MeshCell& cell = cells[index];
        const int c = conductor.at(index), layer = layers.value(cell.layer);
        QPolygonF o = cell.outline;
        if (o.size() > 1 && o.first() == o.last()) o.removeLast();
        const int n = int(o.size());
        QPolygonF out;
        bool more = false;
        for (int i = 0; i < n; ++i) {
            const QPointF a = o.at(i), b = o.at((i + 1) % n);
            out << a;
            // (Along x or y exactly: snapped, a side along one has its two
            //  ends' coordinate the same.)
            const bool alongX = b.y() == a.y(), alongY = b.x() == a.x();
            if (alongX == alongY) continue;
            const auto found = lines.find(Line{layer, c, alongX, alongX ? a.y() : a.x()});
            if (found == lines.end()) continue;
            const std::vector<double>& at = found->second;
            const double from = alongX ? a.x() : a.y(), to = alongX ? b.x() : b.y();
            const auto lo = std::upper_bound(at.begin(), at.end(), std::min(from, to) + tol);
            const auto hi = std::lower_bound(lo, at.end(), std::max(from, to) - tol);
            if (lo == hi) continue;
            QList<double> on(lo, hi);
            if (to < from) std::reverse(on.begin(), on.end());
            for (const double t : std::as_const(on)) {
                const QPointF p = alongX ? QPointF(t, a.y()) : QPointF(a.x(), t);
                if (std::abs(p.x() - out.last().x()) > tol || std::abs(p.y() - out.last().y()) > tol) {
                    out << p;
                    more = true;
                }
            }
        }
        if (more) cell.outline = out;
    }
}

/// A key that two cells sharing an edge agree on, whichever way round they
/// have it: the midpoint, rounded to a nanometre.
QString edgeKey(const QPointF& a, const QPointF& b)
{
    const QPointF middle = (a + b) / 2;
    return QStringLiteral("%1,%2").arg(std::llround(middle.x() * 1e3)).arg(std::llround(middle.y() * 1e3));
}

/*!
 * The metals the solvers take as their two faces, as thickened() decides
 * it, \a stack's flags set so: an Automatic metal as its faces where it is
 * at least a fortieth as thick as the narrowest gap beside it (\a gaps, µm
 * by layer), else a sheet; and a film - under a fiftieth of its narrowest
 * conductor's width - a sheet, however it is asked. What was decided, in
 * \a said. The mesh counts its charges by it too: by the stack's flags
 * alone it counted a film's cells twice - 19,048 "past the dense limit"
 * where the charge solve took 9,524 - and an
 * Automatic metal's once.
 */
QStringList facesDecided(Stack* stack, const QList<Conductor>& conductors, const QMap<QString, double>& gaps, QStringList* said)
{
    // A metal Solved as Automatic: as its two faces where it is at least a
    // fortieth as thick as the narrowest gap beside it (kAutomaticFaces) -
    // where a sheet, which has no thickness in the field and no walls,
    // reads Z0 a per cent and more high (A1 of the open items of 8
    // November) -, else a sheet. Said either way, and decided before
    // anything else reads the stack.
    QStringList words;
    // (Near a threshold the answer steps as a dimension crosses it - a gap
    //  swept across forty times the thickness stepped C12 3.3 %, a width
    //  across fifty times it stepped C 0.86 %
    //  second hour -: said, with how to hold it steady.)
    const auto nearThreshold = [](double ratio, const QString& metal) {
        return ratio > 0.85 && ratio < 1.0 / 0.85
                   ? QLatin1Char(' ') + tr("That is near the threshold: a sweep or an optimization across it steps the answer "
                                           "by up to a few per cent where the choice turns; set %1's Solved as to Sheet or "
                                           "Faces to hold it.")
                                            .arg(metal)
                   : QString();
    };
    for (Layer& l : stack->layers) {
        if (l.kind != Layer::Metal || !l.automatic || l.face != Layer::Whole) continue;
        l.automatic = false;
        const double t = l.thickness * 1e6, gap = gaps.value(l.name, 0);   // (µm, as the mesh)
        if (t > 0 && gap > 0 && t >= kAutomaticFaces * gap) {
            l.twoSheets = true;
            words << tr("%1 is solved as its two faces (Automatic): %2 µm thick beside gaps as narrow as %3 µm, a fortieth "
                        "of them or more.")
                             .arg(l.name).arg(t, 0, 'g', 3).arg(gap, 0, 'g', 3)
                         + nearThreshold(t / (kAutomaticFaces * gap), l.name);
        } else {
            l.twoSheets = false;
            words << (gap > 0 ? tr("%1 is solved as a sheet (Automatic): %2 µm thick, its narrowest gap %3 µm, over forty "
                                   "times as wide.")
                                        .arg(l.name).arg(t, 0, 'g', 3).arg(gap, 0, 'g', 3)
                                    + (t > 0 ? nearThreshold(t / (kAutomaticFaces * gap), l.name) : QString())
                              : tr("%1 is solved as a sheet (Automatic): no gap faces it.").arg(l.name));
        }
    }
    QStringList metals;
    for (const Layer& l : stack->layers)
        if (l.kind == Layer::Metal && l.twoSheets && l.thickness > 0 && l.face == Layer::Whole) metals << l.name;
    // A film - a metal under a fiftieth of its narrowest conductor's width
    // (twice its area over its perimeter) - is one sheet, its thickness in
    // its loss: two faces that close are a pair of sheets whose charges
    // nearly the same potentials cannot tell apart - 0.01 µm on a comb's 1
    // µm fingers took GMRES 2492 iterations at six across, and a
    // twelve-finger comb never converged, where the sheet it is in the limit
    // solves in 22 -, and what they add
    // to it is under a per cent.
    for (qsizetype i = metals.size() - 1; i >= 0; --i) {
        double narrowest = 0;
        for (const Conductor& c : conductors) {
            if (c.layer != metals.at(i)) continue;
            const double around = perimeterOf(c.outline), width = around > 0 ? 2 * areaOf(c.outline) / around : 0;
            if (width > 0 && (narrowest <= 0 || width < narrowest)) narrowest = width;
        }
        const Layer* l = stack->named(metals.at(i));
        const double t = l->thickness * 1e6;   // µm, as the mesh
        if (narrowest > 0 && t < kFacesApart * narrowest) {
            words << tr("%1 is solved as a sheet, its thickness in its loss: %2 µm thick on conductors as narrow as %3 "
                        "µm, it is a film, its two faces as good as one.")
                             .arg(metals.at(i)).arg(t, 0, 'g', 3).arg(narrowest, 0, 'g', 3)
                         + (t / (kFacesApart * narrowest) > 0.85
                                ? QLatin1Char(' ') + tr("That is near the threshold, a fiftieth of the narrowest width: a sweep "
                                                        "or an optimization across it steps the answer by up to a per cent "
                                                        "where the choice turns; set %1's Solved as to Sheet to hold it.")
                                                         .arg(metals.at(i))
                                : QString());
            for (Layer& layer : stack->layers)
                if (layer.name == metals.at(i)) layer.twoSheets = false;
            metals.removeAt(i);
        } else if (narrowest > 0 && t < kFacesApart * narrowest / 0.85) {
            words << tr("%1 is solved as its two faces, %2 µm thick on conductors as narrow as %3 µm: near the threshold of a "
                        "film, a fiftieth of the narrowest width, below which it is a sheet - a sweep or an optimization "
                        "across it steps the answer by up to a per cent where the choice turns; set its Solved as to "
                        "Sheet to hold it.")
                         .arg(metals.at(i)).arg(t, 0, 'g', 3).arg(narrowest, 0, 'g', 3);
        }
    }
    if (said != nullptr) *said << words;
    return metals;
}

} // namespace

QMap<QString, int> Mesh::byLayer() const
{
    QMap<QString, int> counts;
    for (const MeshCell& c : cells) ++counts[c.layer];
    return counts;
}

MeshSettings refinedBy(const MeshSettings& how, double factor)
{
    MeshSettings out = how;
    if (!(factor > 0)) return out;
    if (how.cellsAcross > 0) out.cellsAcross = std::max(1, int(std::lround(how.cellsAcross * factor)));
    out.perWavelength = std::max(1, int(std::lround(how.perWavelength * factor)));
    if (how.maximum > 0) out.maximum = how.maximum / factor;
    out.gapShare = std::min(1.0, how.gapShare / factor);
    return out;
}

MeshSettings meshSettingsOf(const Node& node, const QList<fem::Model::Parameter>& parameters, QStringList* problems)
{
    const fem::Scope scope = scopeOf(parameters);
    MeshSettings how;
    const QJsonValue per = propertyOf(node, QStringLiteral("per_wavelength"));
    if (per.isDouble()) how.perWavelength = per.toInt();
    else if (per.isString() && !per.toString().trimmed().isEmpty()) how.perWavelength = per.toString().toInt();
    const QJsonValue skin = propertyOf(node, QStringLiteral("skin"));
    if (skin.isDouble()) how.skin = skin.toInt();
    const QJsonValue edges = propertyOf(node, QStringLiteral("edges"));
    how.edges = edges.isBool() ? edges.toBool() : how.edges;
    how.maximum = numberIn(node, QStringLiteral("maximum"), scope, 0, tr("Mesh"), problems);
    const QJsonValue across = propertyOf(node, QStringLiteral("cells_across"));
    if (across.isDouble()) how.cellsAcross = across.toInt();
    else if (across.isString() && !across.toString().trimmed().isEmpty()) how.cellsAcross = across.toString().toInt();
    how.stretch = numberIn(node, QStringLiteral("stretch"), scope, how.stretch, tr("Mesh"), problems);
    // (A convergence check's mesh written back: its share at the gaps too -
    //  D2 of the open items.)
    how.gapShare = numberIn(node, QStringLiteral("gap_share"), scope, how.gapShare, tr("Mesh"), problems);
    if (!(how.gapShare > 0 && how.gapShare <= 1)) {
        if (problems != nullptr) *problems << tr("Mesh: the share of a gap its outermost cells come down to is above nought and one at most.");
        how.gapShare = 0.25;
    }
    const QJsonValue most = propertyOf(node, QStringLiteral("most_cells"));
    if (most.isDouble()) how.most = most.toInt();
    else if (most.isString() && !most.toString().trimmed().isEmpty()) how.most = most.toString().toInt();
    if (how.most < 0) {
        if (problems != nullptr) *problems << tr("Mesh: the most cells cannot be fewer than none (0: the solver's own).");
        how.most = 0;
    }
    if (how.cellsAcross < 0) {
        if (problems != nullptr) *problems << tr("Mesh: the cells across a conductor cannot be fewer than none.");
        how.cellsAcross = 0;
    }
    if (how.stretch < 1) {
        if (problems != nullptr) *problems << tr("Mesh: a strip's cells cannot be shorter along it than across (a stretch of 1 at least).");
        how.stretch = 1;
    }
    if (how.perWavelength < 4) {
        if (problems != nullptr)
            *problems << tr("Mesh: %1 cells to a wavelength is too few to carry a current (4 at least).")
                             .arg(how.perWavelength);
        how.perWavelength = std::max(how.perWavelength, 4);
    }
    if (how.maximum < 0) {
        if (problems != nullptr) *problems << tr("Mesh: the largest cell is a length above zero, or nothing.");
        how.maximum = 0;
    }
    if (how.skin < 0) {
        if (problems != nullptr) *problems << tr("Mesh: the layers through the skin depth cannot be fewer than none.");
        how.skin = 0;
    }
    return how;
}

int MeshSettings::mostCells() const
{
    return most > 0 ? most : (compressed || chargeCompressed ? kMostCompressedCells : kMostCharges);
}

MeshCost costOf(int unknowns, bool compressed)
{
    MeshCost cost;
    cost.unknowns = unknowns;
    if (compressed) {
        // Held compressed: some 20 kB an unknown, its low-rank blocks and its
        // near ones, as the solve measured them from 20 000 unknowns to
        // 310 000 (16 to 19 kB).
        cost.matrixBytes = 2e4 * double(unknowns);
        cost.tooBig = unknowns > kMostCompressedUnknowns;
        const double mb = cost.matrixBytes / (1024 * 1024);
        // (Where: a study's Automatic holds a system under its dense limit
        //  dense, which the solve says - this an estimate, not what it did,
        //  NF6 of the open items.)
        cost.said = mb >= 1024 ? tr("%1 unknowns; some %2 GB where they are held compressed").arg(unknowns).arg(mb / 1024, 0, 'f', 1)
                               : tr("%1 unknowns; some %2 MB where they are held compressed").arg(unknowns).arg(mb, 0, 'f', 1);
        if (cost.tooBig)
            cost.said += tr(" - more than this build will set up (%1 unknowns at most). Merge the vias, drop the fill, or "
                            "mesh more coarsely.").arg(kMostCompressedUnknowns);
        return cost;
    }
    // A dense complex matrix: 16 bytes a term, every term kept (the
    // matrix of a mixed-potential formulation is not symmetric).
    cost.matrixBytes = 16.0 * double(unknowns) * double(unknowns);
    cost.tooBig = unknowns > kMostUnknowns;
    const double mb = cost.matrixBytes / (1024 * 1024);
    cost.said = mb >= 1024 ? tr("%1 unknowns; a dense matrix of %2 GB")
                                 .arg(unknowns).arg(mb / 1024, 0, 'f', 1)
                           : tr("%1 unknowns; a dense matrix of %2 MB").arg(unknowns).arg(mb, 0, 'f', 1);
    if (cost.tooBig)
        cost.said += tr(" - more than this build will set up (%1 unknowns at most). Merge the vias, drop the fill, or "
                        "mesh more coarsely.").arg(kMostUnknowns);
    return cost;
}

double topFrequency(const Model& model)
{
    const Node* study = model.root.child(QStringLiteral("rf_study"));
    if (study == nullptr) return 0;
    const fem::Scope scope = scopeOf(model.parameters());
    const double from = numberIn(*study, QStringLiteral("from"), scope, 0, tr("Study"), nullptr);
    const double to = numberIn(*study, QStringLiteral("to"), scope, 0, tr("Study"), nullptr);
    return std::max(from, to);
}

QString chargeCostOf(int cells, bool compressedPast)
{
    // (Past the dense limit, where the solve holds it compressed: said so.)
    if (compressedPast && cells > kMostCharges)
        return cells > kMostCompressedCells
                   ? tr("%1 to solve the charge on - more than this build will solve, compressed (%2 at most): ask for fewer "
                        "cells across a conductor, or a larger largest cell").arg(cells).arg(kMostCompressedCells)
                   : tr("%1 to solve the charge on, past the dense limit (%2): held compressed, solved by iteration")
                         .arg(cells).arg(kMostCharges);
    // A dense real matrix: 8 bytes a term.
    const double mb = 8.0 * double(cells) * double(cells) / (1024 * 1024);
    QString said = mb >= 1024 ? tr("%1 to solve the charge on, a dense matrix of %2 GB").arg(cells).arg(mb / 1024, 0, 'f', 1)
                              : tr("%1 to solve the charge on, a dense matrix of %2 MB").arg(cells).arg(mb, 0, 'f', 1);
    if (cells > kMostCharges)
        said += tr(" - more than this build will solve (%1 at most): ask for fewer cells across a conductor, or a larger "
                   "largest cell").arg(kMostCharges);
    return said;
}

QList<Conductor> withMirrors(const QList<Conductor>& conductors, int axis, double at)
{
    if (axis == 0) return conductors;
    QList<Conductor> out;
    for (const Conductor& given : conductors) {
        // (No strip's centreline: a half is meshed as the whole is, which a
        //  strip's own frame does not keep to.)
        Conductor c = given;
        c.spine.clear();
        c.spineWidth = 0;
        Conductor m = c;
        for (QPointF& p : m.outline) p = axis == 1 ? QPointF(2 * at - p.x(), p.y()) : QPointF(p.x(), 2 * at - p.y());
        std::reverse(m.outline.begin(), m.outline.end());
        const QRectF box = c.outline.boundingRect();
        const double side = axis == 1 ? box.left() : box.top();
        if (std::abs(side - at) <= 1e-9 * std::max({1.0, std::abs(at), box.width(), box.height()})) {
            const QList<QPolygonF> joined = unitedOf({c.outline, m.outline});
            if (joined.size() == 1) {
                Conductor u = c;
                u.outline = joined.first();
                out << u;
                continue;
            }
        }
        out << c << m;
    }
    return out;
}

Mesh meshOf(const QList<Conductor>& given, const MeshSettings& how, const Stack& stack, double top,
            double micrometre)
{
    Mesh mesh;
    const double perUnit = micrometre > 0 ? micrometre : 1e-6;
    // A plane of symmetry: the half meshed as the whole is - the conductors
    // with their mirrors, which join them across the plane - and the cells
    // beyond the plane dropped, any across it cut at it (below). So the
    // half's cells are the whole's: its cut no edge, its widths and its
    // cells across the whole's, the cell budget the whole's, and its two
    // ends as alike as the whole's. Meshed alone, its cut was graded as an
    // edge, its strips cut twice as finely, and the outline the cut leaves
    // split into pieces otherwise than the whole's - two thirds of the
    // whole's cells, a fill no quicker, and S11 and S22 of a line 2e-4 apart
    // (P36 of the open items).
    const int axis = how.symmetryAxis;
    const double plane = how.symmetryAt;
    // (One that reaches the plane united with its mirror - one outline,
    //  split into pieces as the whole's is; the rest beside theirs.)
    const QList<Conductor> conductors = withMirrors(given, axis, plane);
    if (conductors.isEmpty()) {
        mesh.problems << tr("There are no shapes to mesh.");
        return mesh;
    }
    // Shapes on one layer that overlap are refused: each would be meshed
    // whole and the overlap twice - Z 2 % off, the same rectangle twice a
    // capacitance of NaN (overlappingGroups()). The processing joins them,
    // merged or not; this is what comes another way.
    {
        QList<QPolygonF> outlines;
        QStringList layers;
        for (const Conductor& c : given) {
            outlines << c.outline;
            layers << c.layer;
        }
        const QList<QList<int>> groups = overlappingGroups(outlines, layers);
        if (!groups.isEmpty()) {
            const QList<int>& g = groups.first();
            const QRectF box = outlines.at(g.first()).boundingRect();
            mesh.problems << tr("%1 shapes on %2 overlap, about %3, %4 µm: each would be meshed whole and their overlap "
                                "twice. Join them (the processing's Merge touching shapes does), or draw them apart.")
                                 .arg(g.size())
                                 .arg(layers.at(g.first()))
                                 .arg(box.center().x(), 0, 'g', 6)
                                 .arg(box.center().y(), 0, 'g', 6);
            return mesh;
        }
    }

    // The cell size: a wavelength at the highest frequency, in the fastest
    // dielectric of the stack, divided by the cells asked for - and never
    // larger than the largest cell the user allows.
    double epsr = 1;
    for (const Layer& l : stack.layers)
        if (l.kind == Layer::Dielectric || l.kind == Layer::Substrate) epsr = std::max(epsr, l.epsr);
    double size = 0;   // µm
    if (top > 0) {
        const double wavelength = kLightSpeed / (top * std::sqrt(epsr));   // m
        size = wavelength / how.perWavelength / perUnit;
        mesh.lines << tr("A wavelength at %1 GHz in %2 is %3 µm; %4 cells to it.")
                          .arg(top / 1e9, 0, 'g', 3).arg(QStringLiteral("eps_r %1").arg(epsr, 0, 'g', 3))
                          .arg(wavelength / 1e-6, 0, 'g', 3).arg(how.perWavelength);
    }
    if (how.maximum > 0) {
        const double most = how.maximum / perUnit;
        if (size <= 0 || most < size) {
            if (size > 0)
                mesh.lines << tr("The largest cell allowed (%1 µm) is smaller than that.").arg(most, 0, 'g', 3);
            size = most;
        }
    }
    if (size <= 0) {
        mesh.problems << tr("Nothing says how large a cell may be: give the study a frequency, or the mesh a largest "
                            "cell.");
        return mesh;
    }
    mesh.cellSize = size;

    // Each conductor cut against a grid of its own: lines spaced to put at
    // least cellsAcross cells across its narrowest part, a straight
    // strip's stretched along it, and - where the edges are refined - the
    // outermost space of each side halved, which is where its charge
    // crowds. A grid of lines rather than cells divided again keeps every
    // pair of neighbours sharing a whole edge (a quartered cell met its
    // undivided neighbour along half of one, so the inductance's runs of
    // current, which follow shared edges, lost the conductor's rim).
    //
    // A conductor whose every side runs along x or y is cut into the
    // rectangles it is made of first, and each is meshed as a strip of its
    // own: a tee merged into one outline was a grid of the whole, square
    // cells sized by twice its area over its perimeter, and 1120 cells at
    // four across where its two strips are 312. Where two rectangles meet - of one conductor, or two
    // shapes left unmerged - each takes the other's lines along the stretch
    // they share, so that every cell there shares a whole edge with one
    // across it: meshed apart, a stub met its line through one run of
    // current, where two lines happened to coincide. And a side is graded
    // only where it is the conductor's edge, not where it meets another.
    struct Piece {
        int conductor = -1;
        QRectF box;
        bool rectangle = false;        // a rectangle, of a conductor or cut from one
        bool grade[4] = {true, true, true, true};   // its sides at low x, high x, low y, high y
        double gap[4] = {0, 0, 0, 0};                // how far across each side another conductor faces it; 0: none
        QList<double> xs, ys;
        /// A part of its conductor that is no rectangle (piecesOf()): its own
        /// outline, cells clipped to it, and its width; empty, and nought,
        /// for its conductor whole.
        QPolygonF outline;
        double feature = 0;
        /// The way it runs where its slab says (Slab::along); -1: by its box.
        int along = -1;
    };
    QList<Piece> pieces;
    int cutInto = 0, cutFrom = 0, slantedFrom = 0, slantedRectangles = 0, slantedParts = 0, snappedShapes = 0;
    double snappedMost = 0;
    // A strip (Conductor::spine) is no piece: it is cut along its centreline,
    // in cells of each segment's own frame (stripCells()), below - a spiral
    // of 180 segments a turn cut into the parts between its corners was
    // 26,538 cells where the strip needs some 2,000. Not on a metal standing for a round wire, nor with a
    // plane of symmetry: the half is meshed as the whole is.
    QList<StripQuads> quadsOf(conductors.size());
    int stripsCut = 0, stripSegments = 0;
    for (int c = 0; c < conductors.size(); ++c) {
        const Conductor& conductor = conductors.at(c);
        const Layer* metal = stack.named(conductor.layer);
        if (axis != 0 || !conductor.isStrip() || (metal != nullptr && (metal->kind != Layer::Metal || metal->roundWire))) continue;
        quadsOf[c] = stripQuads(conductor.spine, conductor.spineWidth);
        if (quadsOf.at(c).ok()) {
            ++stripsCut;
            stripSegments += int(quadsOf.at(c).lengths.size());
        }
    }
    for (int c = 0; c < conductors.size(); ++c) {
        if (quadsOf.at(c).ok()) continue;
        const QPolygonF& outline = conductors.at(c).outline;
        const QRectF bounds = outline.boundingRect();
        const double area = areaOf(outline);
        const double box = bounds.width() * bounds.height();
        QList<QRectF> boxes;
        if (box > 0 && std::abs(area - box) <= 1e-6 * box) boxes << bounds;
        else boxes = rectanglesOf(outline);
        if (boxes.size() > 1) {
            ++cutFrom;
            cutInto += int(boxes.size());
        }
        if (boxes.isEmpty()) {
            // A side slanted: its rectangles and the parts of it in the boxes
            // between its corners (piecesOf()), else the conductor whole.
            // (Its corners within a twentieth of its width of each other
            //  across the cuts one: piecesOf().)
            const double around = perimeterOf(outline), wide = around > 0 ? 2 * area / around : 0;
            double moved = 0;
            const QList<Slab> slabs = piecesOf(outline, kCornersMerged * wide, &moved, std::min(0.1e-6 / perUnit, 0.01 * wide));
            if (moved > 0) {
                ++snappedShapes;
                snappedMost = std::max(snappedMost, moved);
            }
            if (!slabs.isEmpty()) {
                ++slantedFrom;
                const double reach = std::hypot(bounds.width(), bounds.height());
                for (const Slab& slab : slabs) {
                    Piece piece;
                    piece.conductor = c;
                    piece.box = slab.box;
                    piece.rectangle = slab.rectangle;
                    if (!slab.rectangle) piece.outline = slab.outline;
                    if (!slab.rectangle || slab.along >= 0) piece.feature = slab.feature;
                    // A sliver along a slanted side meshed by how wide the
                    // conductor is there, not by how deep the part is: the
                    // conductor's shortest chord through the part's middle,
                    // along x, along y and across each of its slanted sides
                    // (a strip at an angle its own width). By its own depth,
                    // a part of a circle's cap 0.002 µm deep had its cells so
                    // fine, and its lines taken by its neighbours, that a disc
                    // of 100 µm was 37,000 cells of down to 1e-6 µm² - a review's -, and cells under a
                    // nanometre wide its P57.
                    if (!slab.rectangle && piece.feature > 0) {
                        const QPointF middle = centroidOf(slab.outline, areaOf(slab.outline));
                        double width = std::numeric_limits<double>::infinity();
                        QList<QPointF> ways{QPointF(1, 0), QPointF(0, 1)};
                        for (int k = 0, m = int(slab.outline.size()); k < m; ++k) {
                            const QPointF d = slab.outline.at((k + 1) % m) - slab.outline.at(k);
                            const double length = std::hypot(d.x(), d.y());
                            if (length > 0 && std::abs(d.x()) > 1e-9 * length && std::abs(d.y()) > 1e-9 * length)
                                ways << QPointF(-d.y(), d.x()) / length;
                        }
                        for (const QPointF& way : std::as_const(ways)) {
                            const double chord = breadthAcross({outline}, middle, way, reach);
                            if (chord > 0) width = std::min(width, chord);
                        }
                        // (Where the conductor is four times as wide as the
                        //  part says or more: a part as wide as its own slab,
                        //  a triangle's trapezoid by its mean width, is its
                        //  own.)
                        if (std::isfinite(width) && width >= 4 * piece.feature) piece.feature = width;
                    }
                    piece.along = slab.along;
                    pieces << piece;
                    ++(slab.rectangle ? slantedRectangles : slantedParts);
                }
                continue;
            }
            Piece whole;
            whole.conductor = c;
            whole.box = bounds;
            pieces << whole;
            continue;
        }
        for (const QRectF& b : std::as_const(boxes)) {
            Piece piece;
            piece.conductor = c;
            piece.box = b;
            piece.rectangle = true;
            pieces << piece;
        }
    }
    // Each metal's faces' heights, µm (a sheet's both its bottom), as the
    // solvers place them: which pieces face each other across a thin
    // dielectric. A metal takes no room, but one solved as its two faces
    // its thickness over it (thickened()), which the metals above it are
    // raised by. Left out, a lower plate's top face came out above the
    // upper plate - a MIM's plates of 0.49 and 0.15 µm 40 nm apart as their
    // faces, the gap -0.45 µm - and its cells were never aligned: 52 % low at
    // 5 µm cells, as P28 had been.
    QHash<QString, QPair<double, double>> span;
    {
        double z = 0;
        for (const Layer& l : stack.layers) {
            if (l.kind == Layer::Dielectric || l.kind == Layer::Substrate) z += l.thickness / perUnit;
            if (l.kind != Layer::Metal) continue;
            const double room = l.twoSheets && l.thickness > 0 ? l.thickness / perUnit : 0;
            span.insert(l.name, qMakePair(z, z + room));
            z += room;
        }
    }
    const auto gapBetween = [&](int a, int b) {
        const QString la = conductors.at(pieces.at(a).conductor).layer, lb = conductors.at(pieces.at(b).conductor).layer;
        if (la == lb || !span.contains(la) || !span.contains(lb)) return -1.0;
        const QPair<double, double> sa = span.value(la), sb = span.value(lb);
        return sa.first <= sb.first ? sb.first - sa.second : sa.first - sb.second;
    };
    // Two rectangles on metals far nearer than their cells, overlapping
    // where neither is a plate of the other - a top plate over a bottom one
    // more than twice its size, a bottom plate common to several, a plate
    // banded with its feed: each cut at the other's edges, so that where
    // they overlap each is a piece of its own, its lines shared with the
    // other's below and none crossing the rest of it. Left whole, their
    // cells stayed misaligned and their C read as low as P28's did, with
    // nothing said (the review of 26ff2805). (Wherever the gap is under a
    // tenth of the cells, as the plates' rule: misaligned, a coplanar line's
    // signal 2 µm over its ground read its C 32 % low at 50 µm cells, 25
    // times the gap - the review of 2b157c78, where only 50 times and more
    // was cut.)
    int stackedCut = 0;
    {
        const double sliver = 0.05 * size;
        bool again = true;
        for (int pass = 0; again && pass < 64; ++pass) {
            again = false;
            for (int a = 0; a < pieces.size(); ++a) {
                if (!pieces.at(a).rectangle) continue;
                for (int b = a + 1; b < pieces.size(); ++b) {
                    if (!pieces.at(b).rectangle) continue;
                    const double dz = gapBetween(a, b);
                    if (!(dz > 0) || dz >= 0.1 * size) continue;
                    const QRectF overlap = pieces.at(a).box.intersected(pieces.at(b).box);
                    if (!(overlap.width() > sliver && overlap.height() > sliver)) continue;
                    const double larger = std::max(pieces.at(a).box.width() * pieces.at(a).box.height(),
                                                   pieces.at(b).box.width() * pieces.at(b).box.height());
                    if (overlap.width() * overlap.height() >= 0.5 * larger) continue;
                    // (Its parts: the columns beside the overlap whole, the
                    //  middle one cut above and below it; a part thinner
                    //  than a twentieth of the cells left on the overlap's.)
                    for (const int k : {a, b}) {
                        const QRectF r = pieces.at(k).box;
                        double x0 = r.left(), x1 = r.right(), y0 = r.top(), y1 = r.bottom();
                        if (overlap.left() - r.left() > sliver) x0 = overlap.left();
                        if (r.right() - overlap.right() > sliver) x1 = overlap.right();
                        if (overlap.top() - r.top() > sliver) y0 = overlap.top();
                        if (r.bottom() - overlap.bottom() > sliver) y1 = overlap.bottom();
                        QList<QRectF> parts{QRectF(QPointF(x0, y0), QPointF(x1, y1))};
                        if (x0 > r.left()) parts << QRectF(QPointF(r.left(), r.top()), QPointF(x0, r.bottom()));
                        if (x1 < r.right()) parts << QRectF(QPointF(x1, r.top()), QPointF(r.right(), r.bottom()));
                        if (y0 > r.top()) parts << QRectF(QPointF(x0, r.top()), QPointF(x1, y0));
                        if (y1 < r.bottom()) parts << QRectF(QPointF(x0, y1), QPointF(x1, r.bottom()));
                        if (parts.size() == 1) continue;
                        const Piece whole = pieces.at(k);
                        pieces[k].box = parts.at(0);
                        for (int i = 1; i < parts.size(); ++i) {
                            Piece part = whole;
                            part.box = parts.at(i);
                            pieces << part;
                        }
                        again = true;
                        ++stackedCut;
                    }
                }
            }
        }
    }
    // Which rectangles meet which, along which stretch; and a side wholly
    // met by others is no edge.
    struct Meeting {
        int a, b;
        bool alongX;   // they meet along a line of constant y, so share their x lines
        double lo, hi;
        /// Its ends that are a square corner of one piece's outline - its side
        /// along the stretch ending there in one along the other axis - where
        /// the other's side runs on: lines of both, as two rectangles' ends are
        /// (shareLines()'s ends).
        bool squareLo = false, squareHi = false;
    };
    QList<Meeting> meetings;
    {
        QRectF all;
        for (const Piece& p : std::as_const(pieces)) all = all.united(p.box);
        const double tol = 1e-9 * std::max(1.0, std::max(all.width(), all.height()));
        QList<std::array<double, 4>> met(pieces.size(), {0, 0, 0, 0});
        for (int a = 0; a < pieces.size(); ++a) {
            if (!pieces.at(a).rectangle) continue;
            for (int b = a + 1; b < pieces.size(); ++b) {
                if (!pieces.at(b).rectangle) continue;
                if (conductors.at(pieces.at(a).conductor).layer != conductors.at(pieces.at(b).conductor).layer) continue;
                const QRectF& A = pieces.at(a).box;
                const QRectF& B = pieces.at(b).box;
                const double xLo = std::max(A.left(), B.left()), xHi = std::min(A.right(), B.right());
                const double yLo = std::max(A.top(), B.top()), yHi = std::min(A.bottom(), B.bottom());
                if (xHi - xLo > tol && (std::abs(A.bottom() - B.top()) <= tol || std::abs(A.top() - B.bottom()) <= tol)) {
                    meetings << Meeting{a, b, true, xLo, xHi};
                    const bool aBelow = std::abs(A.bottom() - B.top()) <= tol;
                    met[a][aBelow ? 3 : 2] += xHi - xLo;
                    met[b][aBelow ? 2 : 3] += xHi - xLo;
                }
                if (yHi - yLo > tol && (std::abs(A.right() - B.left()) <= tol || std::abs(A.left() - B.right()) <= tol)) {
                    meetings << Meeting{a, b, false, yLo, yHi};
                    const bool aLeft = std::abs(A.right() - B.left()) <= tol;
                    met[a][aLeft ? 1 : 0] += yHi - yLo;
                    met[b][aLeft ? 0 : 1] += yHi - yLo;
                }
            }
        }
        for (int a = 0; a < pieces.size(); ++a) {
            const QRectF& A = pieces.at(a).box;
            const double side[4] = {A.height(), A.height(), A.width(), A.width()};
            for (int k = 0; k < 4; ++k) pieces[a].grade[k] = met.at(a).at(k) < side[k] - tol;
        }
        // A piece that is no rectangle - its conductor whole, a side of it
        // slanted - meets another along the stretches of their outlines that
        // run along x or y on one line: those lines shared too, so that its
        // clipped cells there share whole edges with the other's. Left out,
        // two touching pieces the union had not joined met through no run of
        // current at all where one had a slanted side. (Its sides stay graded as they were.)
        const auto outlineOf = [&pieces, &conductors](int i) {
            const Piece& p = pieces.at(i);
            return p.rectangle ? QPolygonF(p.box) : !p.outline.isEmpty() ? p.outline : conductors.at(p.conductor).outline;
        };
        for (int a = 0; a < pieces.size(); ++a)
            for (int b = a + 1; b < pieces.size(); ++b) {
                if (pieces.at(a).rectangle && pieces.at(b).rectangle) continue;
                if (conductors.at(pieces.at(a).conductor).layer != conductors.at(pieces.at(b).conductor).layer) continue;
                if (!pieces.at(a).box.adjusted(-tol, -tol, tol, tol).intersects(pieces.at(b).box)) continue;
                const QPolygonF A = outlineOf(a), B = outlineOf(b);
                for (int i = 0, na = int(A.size()); i < na; ++i) {
                    const QPointF p = A.at(i), q = A.at((i + 1) % na);
                    const bool alongX = std::abs(q.y() - p.y()) <= tol, alongY = std::abs(q.x() - p.x()) <= tol;
                    if (alongX && alongY) continue;
                    if (!alongX && !alongY) {
                        // A slanted stretch shared: both pieces cut at the same
                        // points along it - every line of either that crosses
                        // it, along x and along y, made both's -, so that each
                        // cell clipped by it on one side meets one on the
                        // other along the same piece of it. Met in part only,
                        // two pieces touching along a slant were joined where
                        // their grids happened to agree.
                        const QPointF d = q - p;
                        const double len = std::hypot(d.x(), d.y());
                        const QPointF unit = d / len, normal(-unit.y(), unit.x());
                        for (int j = 0, nb = int(B.size()); j < nb; ++j) {
                            const QPointF u = B.at(j) - p, w = B.at((j + 1) % nb) - p;
                            if (std::abs(u.x() * normal.x() + u.y() * normal.y()) > tol
                                || std::abs(w.x() * normal.x() + w.y() * normal.y()) > tol)
                                continue;
                            const double tu = u.x() * unit.x() + u.y() * unit.y(), tw = w.x() * unit.x() + w.y() * unit.y();
                            const double t0 = std::max(0.0, std::min(tu, tw)), t1 = std::min(len, std::max(tu, tw));
                            if (t1 - t0 <= tol) continue;
                            const QPointF e0 = p + unit * t0, e1 = p + unit * t1;
                            meetings << Meeting{a, b, true, std::min(e0.x(), e1.x()), std::max(e0.x(), e1.x())};
                            meetings << Meeting{a, b, false, std::min(e0.y(), e1.y()), std::max(e0.y(), e1.y())};
                        }
                        continue;
                    }
                    for (int j = 0, nb = int(B.size()); j < nb; ++j) {
                        const QPointF u = B.at(j), w = B.at((j + 1) % nb);
                        if (alongX ? (std::abs(u.y() - p.y()) > tol || std::abs(w.y() - p.y()) > tol)
                                   : (std::abs(u.x() - p.x()) > tol || std::abs(w.x() - p.x()) > tol))
                            continue;
                        const double lo = std::max(alongX ? std::min(p.x(), q.x()) : std::min(p.y(), q.y()),
                                                   alongX ? std::min(u.x(), w.x()) : std::min(u.y(), w.y()));
                        const double hi = std::min(alongX ? std::max(p.x(), q.x()) : std::max(p.y(), q.y()),
                                                   alongX ? std::max(u.x(), w.x()) : std::max(u.y(), w.y()));
                        if (hi - lo <= tol) continue;
                        // (A square corner at an end, where the other's side
                        //  runs on: the piece whose side ends there turns onto
                        //  the other axis, its cells' corner there, and the
                        //  other's cells must break there too. Left out, a hexagon's cells beside
                        //  a square hole's corners met no cell across x = 60
                        //  for 17.5 µm: the pieces under and over the hole
                        //  ended at its corners, and the piece beside them ran
                        //  on - the review of 27cc932c. Where a slanted side
                        //  meets the stretch it is no line of either: a taper
                        //  beside a line put its corners into the line, which
                        //  must be meshed as drawn alone.)
                        const auto squareAt = [tol, alongX](const QPolygonF& poly, double t, double c) {
                            const QPointF at = alongX ? QPointF(t, c) : QPointF(c, t);
                            const int n = int(poly.size());
                            for (int k = 0; k < n; ++k) {
                                if (std::abs(poly.at(k).x() - at.x()) > tol || std::abs(poly.at(k).y() - at.y()) > tol) continue;
                                for (const QPointF& next : {poly.at((k + n - 1) % n), poly.at((k + 1) % n)}) {
                                    const double along = alongX ? std::abs(next.x() - at.x()) : std::abs(next.y() - at.y());
                                    const double across = alongX ? std::abs(next.y() - at.y()) : std::abs(next.x() - at.x());
                                    if (along <= tol && across > tol) return true;
                                }
                            }
                            return false;
                        };
                        // (An end where one's side ends and the other's runs
                        //  on past it: where both end, neither's cells go on.)
                        const double c = alongX ? p.y() : p.x();
                        const double aLo = alongX ? std::min(p.x(), q.x()) : std::min(p.y(), q.y());
                        const double aHi = alongX ? std::max(p.x(), q.x()) : std::max(p.y(), q.y());
                        const double bLo = alongX ? std::min(u.x(), w.x()) : std::min(u.y(), w.y());
                        const double bHi = alongX ? std::max(u.x(), w.x()) : std::max(u.y(), w.y());
                        Meeting met{a, b, alongX, lo, hi};
                        met.squareLo = (aLo < lo - tol && squareAt(B, lo, c)) || (bLo < lo - tol && squareAt(A, lo, c));
                        met.squareHi = (aHi > hi + tol && squareAt(B, hi, c)) || (bHi > hi + tol && squareAt(A, hi, c));
                        meetings << met;
                    }
                }
            }
    }
    // Pieces on two metals one over the other, a thin dielectric apart - far
    // nearer than their cells are wide, a tenth of them - and plates of each
    // other, their overlap half the larger or more: their lines shared where
    // they overlap, both ways, so that their cells face cell for cell. (Not
    // a track crossing a shield, a winding over a winding, fill under a
    // line, a coplanar line over a lower ground: shared there, each piece
    // took the union of the others' lines across it, a tartan of cells - the
    // review of f13694ea; there the gap is no small share of the cells.) Their
    // charges must mirror across the gap, which uniform cells on two grids
    // cannot: a MIM's plates 40 nm apart read their C 52 % low at cells of
    // 5 µm, 25 % at 2.5, the bottom plate's cells under the top's
    // misaligned (P28 of the open items).
    int stackedShared = 0;
    struct Facing {
        int a, b;
        double dz;   // µm
    };
    QList<Facing> facingPairs;    // (each pair facing across such a gap)
    QList<Facing> unaligned;      // (and those whose cells nothing aligns: said where they are coarse)
    {
        QRectF all;
        for (const Piece& p : std::as_const(pieces)) all = all.united(p.box);
        const double tol = 1e-9 * std::max(1.0, std::max(all.width(), all.height()));
        for (int a = 0; a < pieces.size(); ++a) {
            for (int b = a + 1; b < pieces.size(); ++b) {
                const double dz = gapBetween(a, b);
                if (!(dz > 0) || dz >= 0.1 * size) continue;
                const QRectF& A = pieces.at(a).box;
                const QRectF& B = pieces.at(b).box;
                const double xLo = std::max(A.left(), B.left()), xHi = std::min(A.right(), B.right());
                const double yLo = std::max(A.top(), B.top()), yHi = std::min(A.bottom(), B.bottom());
                if (xHi - xLo <= tol || yHi - yLo <= tol) continue;
                const double larger = std::max(A.width() * A.height(), B.width() * B.height());
                // (A part of a slanted side, or a pair the cut above left
                //  apart: its cells misaligned, said where they are coarse.)
                if (!pieces.at(a).rectangle || !pieces.at(b).rectangle || (xHi - xLo) * (yHi - yLo) < 0.5 * larger) {
                    unaligned << Facing{a, b, dz};
                    continue;
                }
                meetings << Meeting{a, b, true, xLo, xHi} << Meeting{a, b, false, yLo, yHi};
                ++stackedShared;
                facingPairs << Facing{a, b, dz};
            }
        }
    }
    if (stackedShared > 0)
        mesh.lines << tr("%1 pair(s) of pieces on two metals face each other across a gap narrower than their cells: their "
                         "cells are cut alike where they overlap, so that their charges face cell for cell.")
                          .arg(stackedShared);
    if (stackedCut > 0)
        mesh.lines << tr("%1 piece(s) cut at the edges of a piece over or under them, a thin gap apart, so that where they "
                         "overlap their cells face each other and no line of theirs crosses the rest.")
                          .arg(stackedCut);
    if (cutFrom > 0)
        mesh.lines << tr("%1 shape(s) cut into the %2 rectangle(s) they are made of, each meshed as a strip of its own; "
                         "where two rectangles meet, their cells meet edge to edge.")
                          .arg(cutFrom).arg(cutInto);
    if (slantedFrom > 0)
        mesh.lines << tr("%1 shape(s) with slanted sides cut at their corners into %2 rectangle(s), each meshed as a strip "
                         "of its own, and %3 part(s) along the slanted sides, each meshed by its own width; where two "
                         "meet, their cells meet edge to edge.")
                          .arg(slantedFrom).arg(slantedRectangles).arg(slantedParts);
    if (stripsCut > 0)
        mesh.lines << tr("%1 strip(s) cut along their centrelines, %2 segment(s) in all: each segment cut across its width and "
                         "along its length in cells of its own frame, the next segment's meeting them edge to edge at the "
                         "mitre between them.")
                          .arg(stripsCut).arg(stripSegments);
    if (snappedShapes > 0)
        mesh.lines << tr("%1 of them with a curve's corners nearer each other across the cuts than a twentieth of the shape's "
                         "width, put on one cut, each slid along a side of its own or moved across by the curves' tolerance, the "
                         "outline %2 µm at most off itself: no part is a sliver between two corners a hair apart.")
                          .arg(snappedShapes).arg(snappedMost, 0, 'g', 3);
    // Where each side faces another conductor of its layer, and how far
    // across: the charge crowds on both edges of a narrow gap, and halving
    // the outermost cells once - the edges' grading - leaves a gap of 6 µm
    // between conductors of 50 and 150 µm with one cell of 3 µm on one side
    // and of 9 on the other. A coplanar line so on thin-film lithium niobate
    // came out 10 % under its capacitance in vacuum, 12 % under its own
    // conformal mapping; its edge cells halved
    // down to a quarter of the gap it is a per cent from either, and the
    // cells across hardly matter. The current crowds there too: a measured
    // coplanar line on alumina, 25 µm gaps beside a 50 µm signal and its
    // 270 µm grounds, the grounds' outermost cells 45 µm wide, lost 19 %
    // under its 2D eddy-current solve in a full-wave solve, and 1 % under at
    // 10 GHz to 9 % over at 60 on the graded cells. (Pieces swept in order
    // of their left edges: only those within twice the largest cell can face
    // one within reach of its grading.)
    const auto outlineOfPiece = [&conductors](const Piece& p) {
        return p.rectangle ? QPolygonF(p.box) : !p.outline.isEmpty() ? p.outline : conductors.at(p.conductor).outline;
    };
    if (how.edges && how.gaps) {
        QList<int> order(pieces.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&pieces](int a, int b) { return pieces.at(a).box.left() < pieces.at(b).box.left(); });
        const double reach = size / (2 * how.gapShare);
        for (int oi = 0; oi < order.size(); ++oi) {
            Piece& p = pieces[order.at(oi)];
            const QRectF& A = p.box;
            const QString& layer = conductors.at(p.conductor).layer;
            for (int oj = oi + 1; oj < order.size(); ++oj) {
                Piece& q = pieces[order.at(oj)];
                const QRectF& B = q.box;
                if (B.left() > A.right() + reach) break;
                if (conductors.at(q.conductor).layer != layer) continue;
                const auto keep = [](double* at, double g) {
                    if (g > 0 && (*at <= 0 || g < *at)) *at = g;
                };
                const bool acrossY0 = B.bottom() > A.top() && B.top() < A.bottom();   // they share some y: face across x
                const bool acrossX0 = B.right() > A.left() && B.left() < A.right();   // they share some x: face across y
                // (Two pieces of one conductor with that conductor between
                //  them - a line and its own pad past its taper - face no gap.)
                const auto filled = [&](bool alongX) {
                    if (p.conductor != q.conductor) return false;
                    const QPointF mid = alongX ? QPointF(0.5 * (std::max(A.left(), B.left()) + std::min(A.right(), B.right())),
                                                         0.5 * (std::min(A.bottom(), B.bottom()) + std::max(A.top(), B.top())))
                                               : QPointF(0.5 * (std::min(A.right(), B.right()) + std::max(A.left(), B.left())),
                                                         0.5 * (std::max(A.top(), B.top()) + std::min(A.bottom(), B.bottom())));
                    return conductors.at(p.conductor).outline.containsPoint(mid, Qt::OddEvenFill);
                };
                const bool acrossY = acrossY0 && !filled(false);
                const bool acrossX = acrossX0 && !filled(true);
                // (Between two rectangles the gap is their boxes'; where
                //  either is a slanted side's part, its box's corner may be
                //  nowhere near the other, and the gap is how far apart
                //  their outlines are - the parts of an octagonal spiral's
                //  turns, 2 µm apart, were graded to boxes 0.73 µm apart.)
                const bool slanted = !p.rectangle || !q.rectangle;
                double apart = -1;
                const auto gapOf = [&](double boxes) {
                    if (!slanted || boxes <= 0) return boxes;
                    if (apart < 0) apart = outlineDistance(outlineOfPiece(p), outlineOfPiece(q));
                    return std::max(boxes, apart);
                };
                if (acrossY && B.left() >= A.right()) {
                    keep(&p.gap[1], gapOf(B.left() - A.right()));
                    keep(&q.gap[0], gapOf(B.left() - A.right()));
                }
                if (acrossY && A.left() >= B.right()) {
                    keep(&p.gap[0], gapOf(A.left() - B.right()));
                    keep(&q.gap[1], gapOf(A.left() - B.right()));
                }
                if (acrossX && B.top() >= A.bottom()) {
                    keep(&p.gap[3], gapOf(B.top() - A.bottom()));
                    keep(&q.gap[2], gapOf(B.top() - A.bottom()));
                }
                if (acrossX && A.top() >= B.bottom()) {
                    keep(&p.gap[2], gapOf(A.top() - B.bottom()));
                    keep(&q.gap[3], gapOf(A.top() - B.bottom()));
                }
            }
        }
        for (Piece& p : pieces)
            for (int k = 0; k < 4; ++k)
                if (!p.grade[k] || p.gap[k] >= reach) p.gap[k] = 0;
    }
    double narrowest = 0, finest = 0;
    int strips = 0;
    // The conductors cut with \a perFeature cells across each: false where
    // one of them would take more cells than a mesh may hold, \a many then
    // how many it would have taken.
    // As many cells as the solver it is for holds, before fewer are put
    // across a conductor; and a piece of more than four times that, or of
    // the old bound of a dense full-wave matrix where that is more, not cut
    // at all.
    const int most = how.mostCells();
    const double pieceMost = std::max(double(kMostUnknowns), double(most));
    int facing = 0;
    QList<int> cellConductor;   // (each cell's conductor, for the check of touching pieces below)
    // A layer standing for a round wire (a bond wire's strips and legs,
    // Layer::roundWire) is one cell across, its edges not halved: a strip's
    // width and conductivity are a round wire's with its current even across
    // it, and a leg's square one bar's with its skin round it. Cut finer, a
    // strip's current crowded to its edges, as a flat strip's does and a
    // round wire's does not, and a leg was a bundle of thin vias each with
    // its own skin: at six across, with a pad's cells, the wire's R read 1.8
    // to 2.3 times a round wire's (P34 of the open items).
    QSet<QString> roundWires;
    for (const Layer& l : stack.layers)
        if (l.roundWire) roundWires << l.name;
    const auto cut = [&mesh, &pieces, &meetings, &conductors, &how, &narrowest, &finest, &strips, &facing, &cellConductor,
                      &roundWires, &quadsOf, size, pieceMost](int perFeature, double* many) {
        mesh.cells.clear();
        cellConductor.clear();
        narrowest = finest = 0;
        strips = 0;
        facing = 0;
        // (Over every piece too, not each alone: 2,000 pads of 20 µm at a
        //  largest cell of 0.1 µm, each under the bound, were cut into
        //  8e7 cells, 29 GB and 18 s before the whole was found too many.)
        double total = 0;
        for (Piece& piece : pieces) {
            const Conductor& conductor = conductors.at(piece.conductor);
            const QRectF bounds = piece.box;
            const double narrow = std::min(bounds.width(), bounds.height());
            // How wide the conductor is where it is narrowest: a rectangle's
            // short side, else twice its area over its perimeter - which is
            // a trace's width for a trace of any shape, a spiral's included.
            const QPolygonF& shape = piece.outline.isEmpty() ? conductor.outline : piece.outline;
            const double area = piece.rectangle ? bounds.width() * bounds.height() : areaOf(shape);
            const double perimeter = piece.rectangle ? 0 : perimeterOf(shape);
            const double feature = piece.feature > 0 ? piece.feature : piece.rectangle ? narrow
                                                                                        : (perimeter > 0 ? 2 * area / perimeter : narrow);
            const bool round = roundWires.contains(conductor.layer);
            double across = size;
            if (round && feature > 0) across = feature * (1 + 1e-9);
            else if (perFeature > 0 && feature > 0) across = std::min(across, feature / perFeature);
            // (Its length the way its slab says, where it says: a rectangle
            //  cut thin between two slabs is no strip along its thin side.)
            const bool alongX = piece.along >= 0 ? piece.along == 0 : bounds.width() >= bounds.height();
            const bool strip = piece.rectangle && (alongX ? bounds.width() : bounds.height()) >= 3 * feature;
            double along = across;
            if (strip && how.stretch > 1) along = std::min(size, how.stretch * across);
            const double stepX = strip ? (alongX ? along : across) : across;
            const double stepY = strip ? (alongX ? across : along) : across;
            // (Counted before the lines are made: a fine grid over a large
            // layout would be millions of them.)
            const double squares = std::ceil(bounds.width() / stepX) * std::ceil(bounds.height() / stepY);
            total += squares;
            if (squares > 4.0 * pieceMost) {
                *many = squares;
                return false;
            }
            // (Past the bound, the rest counted, no lines made: the layout's
            //  number said, not where the sum crossed it - 2.8e+05 for the
            //  pads' 8.16e+07, the review of 639258e6.)
            if (total > 4.0 * pieceMost) continue;
            static const double none[4] = {0, 0, 0, 0};
            const double* gap = round ? none : piece.gap;
            const bool edges = how.edges && !round;
            piece.xs = gridLines(bounds.left(), bounds.width(), stepX, edges && piece.grade[0], edges && piece.grade[1],
                                 how.gapShare * gap[0], how.gapShare * gap[1]);
            piece.ys = gridLines(bounds.top(), bounds.height(), stepY, edges && piece.grade[2], edges && piece.grade[3],
                                 how.gapShare * gap[2], how.gapShare * gap[3]);
            if (round) continue;
            // (The sides graded past the edges' one halving, for the line
            //  that says so: those whose quarter gap is narrower than it.)
            for (int k = 0; k < 4 && how.edges; ++k) {
                const double span = k < 2 ? bounds.width() : bounds.height(), step = k < 2 ? stepX : stepY;
                const double outermost = span / std::max(1, int(std::ceil(span / step - 1e-9))) / 2;
                if (gap[k] > 0 && how.gapShare * gap[k] < outermost) ++facing;
            }
            if (narrowest <= 0 || feature < narrowest) narrowest = feature;
            if (finest <= 0 || across < finest) finest = across;
            if (strip) ++strips;
        }
        if (total > 4.0 * pieceMost) {
            *many = total;
            return false;
        }
        // Each meeting's lines made both pieces', until none gains any: a
        // line one takes may lie where it meets a third.
        QRectF all;
        for (const Piece& p : std::as_const(pieces)) all = all.united(p.box);
        const double tol = 1e-9 * std::max(1.0, std::max(all.width(), all.height()));
        // Lines a quarter of the finest space of any piece's own grid apart,
        // or nearer, are one where two pieces meet (shareLines()): no cell
        // made by sharing is narrower than a quarter of the narrowest one
        // asked for.
        // (Of the spaces a piece's own grid cuts it into, not of a piece
        //  thinner that way than the finest cell any piece asked for - a part
        //  of a curve's cap 0.002 µm deep, its edge's grading halving its
        //  depth, no space anyone asked for. Taken as the finest, next to
        //  nothing was merged, and a ring of 100 µm with feeds meshed at 5 µm
        //  was 32,000 cells, 19,700 with it.)
        double finestSpace = 0;
        for (const Piece& p : std::as_const(pieces))
            for (const QList<double>* lines : {&p.xs, &p.ys})
                for (int k = 1; lines->size() > 1 && lines->last() - lines->first() >= 0.5 * finest && k < lines->size(); ++k)
                    if (const double d = lines->at(k) - lines->at(k - 1); d > tol && (finestSpace <= 0 || d < finestSpace))
                        finestSpace = d;
        const double merge = kMergeShare * finestSpace;
        const auto shareAll = [&pieces, &meetings, tol, merge](bool rectanglesOnly) {
            bool gained = !meetings.isEmpty();
            for (int round = 0; gained && round < 64; ++round) {
                gained = false;
                for (const Meeting& m : std::as_const(meetings)) {
                    Piece& a = pieces[m.a];
                    Piece& b = pieces[m.b];
                    const bool rectangles = a.rectangle && b.rectangle;
                    if (rectanglesOnly && !rectangles) continue;
                    gained = (m.alongX ? shareLines(a.xs, b.xs, m.lo, m.hi, tol, rectangles, merge)
                                       : shareLines(a.ys, b.ys, m.lo, m.hi, tol, rectangles, merge))
                             || gained;
                    // (Its square ends both's lines, where each reaches.)
                    if (!rectangles)
                        for (const auto& [square, end] : {std::pair{m.squareLo, m.lo}, std::pair{m.squareHi, m.hi}}) {
                            if (!square) continue;
                            for (QList<double>* lines : m.alongX ? std::array{&a.xs, &b.xs} : std::array{&a.ys, &b.ys}) {
                                if (lines->size() < 2 || end <= lines->first() + tol || end >= lines->last() - tol) continue;
                                bool known = false;
                                for (const double x : std::as_const(*lines)) known = known || std::abs(x - end) <= tol;
                                if (known) continue;
                                lines->insert(std::lower_bound(lines->begin(), lines->end(), end), end);
                                gained = true;
                            }
                        }
                }
            }
        };
        shareAll(true);
        // Where a slanted side's trapezoid meets the rest, the lines go one
        // way: from the straight strips - a line, which a polygon's line
        // must be meshed as when drawn alone - to the trapezoids beside
        // them, and from those to the other rectangles (a pad), each piece
        // taking its stronger neighbours' lines where it meets them in place
        // of its own. Shared both ways, a pad's lines came through its taper
        // into the line across its width. Then shared both ways, so that cells meet edge to edge
        // whatever was left.
        bool parts = false;
        for (const Piece& p : std::as_const(pieces)) parts = parts || (!p.rectangle && !p.outline.isEmpty());
        if (parts) {
            const auto isStrip = [](const Piece& p) {
                const double lo = std::min(p.box.width(), p.box.height()), hi = std::max(p.box.width(), p.box.height());
                return p.rectangle && hi >= 3 * lo;
            };
            const auto rank = [&isStrip](const Piece& p) { return isStrip(p) ? 0 : !p.rectangle ? 1 : 2; };
            const auto widthOf = [](const Piece& p) {
                return p.rectangle ? std::min(p.box.width(), p.box.height()) : p.feature;
            };
            QList<int> order(pieces.size());
            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                const Piece& p = pieces.at(a);
                const Piece& q = pieces.at(b);
                if (rank(p) != rank(q)) return rank(p) < rank(q);
                if (rank(p) == 0) return std::max(p.box.width(), p.box.height()) > std::max(q.box.width(), q.box.height());
                return widthOf(p) < widthOf(q);
            });
            QList<int> place(pieces.size());
            for (int k = 0; k < order.size(); ++k) place[order.at(k)] = k;
            // Each piece's stretches already taken from a stronger one, each way.
            QList<QList<std::pair<double, double>>> lockedX(pieces.size()), lockedY(pieces.size());
            const auto inside = [tol](const QList<std::pair<double, double>>& locks, double x) {
                for (const auto& [lo, hi] : locks)
                    if (x > lo + tol && x < hi - tol) return true;
                return false;
            };
            for (const int p : std::as_const(order)) {
                // Its meetings with stronger pieces, the strongest first.
                QList<const Meeting*> mine;
                for (const Meeting& m : std::as_const(meetings)) {
                    const int q = m.a == p ? m.b : m.b == p ? m.a : -1;
                    if (q < 0 || place.at(q) > place.at(p)) continue;
                    if (pieces.at(p).rectangle && pieces.at(q).rectangle) continue;
                    mine << &m;
                }
                std::stable_sort(mine.begin(), mine.end(), [&](const Meeting* x, const Meeting* y) {
                    return place.at(x->a == p ? x->b : x->a) < place.at(y->a == p ? y->b : y->a);
                });
                for (const Meeting* m : std::as_const(mine)) {
                    const int q = m->a == p ? m->b : m->a;
                    QList<double>& lines = m->alongX ? pieces[p].xs : pieces[p].ys;
                    const QList<double>& theirs = m->alongX ? pieces.at(q).xs : pieces.at(q).ys;
                    QList<std::pair<double, double>>& locks = m->alongX ? lockedX[p] : lockedY[p];
                    QList<double> next;
                    for (const double x : std::as_const(lines))
                        if (x <= m->lo + tol || x >= m->hi - tol || inside(locks, x)) next << x;
                    for (const double x : theirs)
                        if (x > m->lo + tol && x < m->hi - tol && !inside(locks, x)) next << x;
                    std::sort(next.begin(), next.end());
                    lines.clear();
                    for (const double x : std::as_const(next))
                        if (lines.isEmpty() || x - lines.last() > tol) lines << x;
                    locks << std::make_pair(m->lo, m->hi);
                }
            }
        }
        shareAll(false);
        // (The squares of every piece counted before any is made.)
        double squaresAll = 0;
        for (const Piece& piece : std::as_const(pieces)) squaresAll += double(piece.xs.size() - 1) * double(piece.ys.size() - 1);
        if (squaresAll > 4.0 * pieceMost) {
            *many = squaresAll;
            return false;
        }
        for (const Piece& piece : std::as_const(pieces)) {
            const Conductor& conductor = conductors.at(piece.conductor);
            const QList<double>& xs = piece.xs;
            const QList<double>& ys = piece.ys;
            for (int i = 0; i + 1 < xs.size(); ++i)
                for (int j = 0; j + 1 < ys.size(); ++j) {
                    const QRectF square(QPointF(xs.at(i), ys.at(j)), QPointF(xs.at(i + 1), ys.at(j + 1)));
                    const double full = square.width() * square.height();
                    const QList<QPolygonF> cut = piece.rectangle ? QList<QPolygonF>{QPolygonF(square)}
                                                                 : clipTo({piece.outline.isEmpty() ? conductor.outline : piece.outline},
                                                                          QPolygonF(square));
                    for (const QPolygonF& part : cut) {
                        const double a = piece.rectangle ? full : areaOf(part);
                        if (a <= 0) continue;
                        MeshCell cell;
                        cell.layer = conductor.layer;
                        cell.share = conductor.share;
                        cell.outline = part;
                        // (A cell cut by a slanted side is held at its
                        //  centroid: its box's middle may be on its edge - a
                        //  right triangle's is on its hypotenuse -, and a run
                        //  or a stub to it from that edge had no length, its
                        //  conductance without bound.)
                        cell.centre = piece.rectangle || std::abs(a - full) <= full * 1e-9 ? square.center()
                                                                                           : centroidOf(part, a);
                        cell.area = a;
                        // On the edge where the cut took something off, or
                        // where the square touches the conductor's bounds.
                        cell.onEdge = std::abs(a - full) > full * 1e-6 || i == 0 || j == 0 || i + 2 == xs.size()
                                      || j + 2 == ys.size();
                        mesh.cells << cell;
                        cellConductor << piece.conductor;
                    }
                }
        }
        for (int c = 0; c < conductors.size(); ++c) {
            if (!quadsOf.at(c).ok()) continue;
            double across = 0;
            bool strip = false;
            const QList<MeshCell> cells =
                stripCells(conductors.at(c), quadsOf.at(c), size, perFeature, how.stretch, how.edges, &across, &strip);
            if (double(mesh.cells.size() + cells.size()) > 4.0 * pieceMost) {
                *many = double(mesh.cells.size() + cells.size());
                return false;
            }
            for (const MeshCell& cell : cells) {
                mesh.cells << cell;
                cellConductor << c;
            }
            const double feature = conductors.at(c).spineWidth;
            if (narrowest <= 0 || feature < narrowest) narrowest = feature;
            if (finest <= 0 || across < finest) finest = across;
            if (strip) ++strips;
        }
        *many = double(mesh.cells.size());
        return true;
    };
    // As many across as asked, or as many as the solver will take: a large
    // layout at ten across a narrow trace is tens of thousands of cells,
    // and refusing it outright left a user nothing to solve (the heater
    // example came out at 72 024). Fewer across is said, with why.
    int perFeature = how.cellsAcross;
    double many = 0;
    bool fits = cut(perFeature, &many) && many <= most;
    while (!fits && perFeature > 1) {
        const double was = many;
        const int fewer = std::max(1, int(std::floor(perFeature * std::sqrt(double(most) / std::max(1.0, many)))));
        const int tried = std::min(perFeature - 1, fewer);
        double now = 0;
        const bool cutOk = cut(tried, &now);
        // (Where the largest cell sets the size and not the cells across,
        // fewer across changes nothing: no use going on.)
        if (cutOk && now >= was) {
            perFeature = tried;
            many = now;
            break;
        }
        perFeature = tried;
        many = now;
        fits = cutOk && many <= most;
    }
    if (!fits && many > 4.0 * pieceMost) {
        mesh.cells.clear();
        mesh.problems << tr("The layout would take %1 cells of %2 µm even at one across a conductor: ask for a larger "
                            "largest cell, or a lower top frequency.")
                             .arg(many, 0, 'g', 3).arg(size, 0, 'g', 3);
        return mesh;
    }
    // (A warning too, in the solve's answer: said in the tab's notes alone,
    //  a model asking for 8 across solved at 2 without a word in it - D2 of
    //  the open items.)
    if (perFeature < how.cellsAcross && fits) {
        const QString w = tr("%1 cells across a conductor would be more than the %2 this build solves: %3 across instead "
                             "(Most cells on the Mesh node raises it).")
                              .arg(how.cellsAcross).arg(most).arg(perFeature);
        mesh.lines << w;
        mesh.warnings << w;
    } else if (perFeature < how.cellsAcross) {
        // (Which sets the size: at one across, a conductor narrower than the
        //  largest cell has cells as narrow as it, whatever the largest cell
        //  - a wedge 0.5 µm at its base was told to ask for a larger one, and
        //  a largest cell of 1000 µm made the same 9,992 cells as one of 20.)
        const QString w = finest > 0 && finest < size * (1 - 1e-9)
                              ? tr("Fewer cells across a conductor do not bring the mesh within the %1 this build solves: at one "
                                   "cell across, its narrowest conductor, %2 µm across, has cells of %3 µm, whatever the largest "
                                   "cell - widen or drop what is that narrow (a sliver, a wedge's point), or raise Most cells on "
                                   "the Mesh node.")
                                    .arg(most).arg(narrowest, 0, 'g', 3).arg(finest, 0, 'g', 3)
                              : tr("Fewer cells across a conductor do not bring the mesh within the %1 this build solves: the "
                                   "largest cell, or the wavelength, sets the size there - ask for a larger largest cell.")
                                    .arg(most);
        mesh.lines << w;
        mesh.warnings << w;
    }
    if (perFeature > 0 && narrowest > 0) {
        mesh.narrowest = narrowest;
        mesh.across = perFeature;
        mesh.acrossCell = finest;
    }
    if (perFeature > 0 && narrowest > 0)
        mesh.lines << tr("%1 cell(s) across the narrowest conductor (%2 µm across): cells of %3 µm there%4.")
                          .arg(perFeature).arg(narrowest, 0, 'g', 4).arg(finest, 0, 'g', 3)
                          .arg(strips > 0 ? tr(", up to %1 times as long along a straight strip").arg(how.stretch, 0, 'g', 3)
                                          : QString());
    if (how.edges) mesh.lines << tr("The outermost cells of each conductor's sides are half as wide, where its charge "
                                    "crowds.");
    mesh.facing = facing;
    // (Aligned, a MIM's C at cells 125 times its gap is within 1 %, at 250
    //  times some 5 % low: past 150 times, said.)
    // (Pair by pair, each pair's own gap against its own cells: a fine MIM
    //  beside a coarse crossing was read as the one's gap under the other's
    //  cells - the review of f13694ea.)
    double narrowestFacing = 0, facingCell = 0;
    for (const Facing& pair : std::as_const(facingPairs)) {
        double widest = 0;
        for (const int k : {pair.a, pair.b})
            for (const QList<double>* lines : {&pieces.at(k).xs, &pieces.at(k).ys})
                for (int i = 1; i < lines->size(); ++i) widest = std::max(widest, lines->at(i) - lines->at(i - 1));
        if (narrowestFacing <= 0 || widest / pair.dz > facingCell / narrowestFacing) {
            narrowestFacing = pair.dz;
            facingCell = widest;
        }
    }
    // (Pieces whose cells nothing aligns - a slanted side's parts - coarse
    //  against their gap: said, their C reading low as a MIM's did. From 10
    //  times the gap: misaligned cells cost some ten times what aligned ones
    //  do, a third of the C at 25 times - the review of 2b157c78.)
    double unalignedGap = 0, unalignedCell = 0;
    for (const Facing& pair : std::as_const(unaligned)) {
        double widest = 0;
        for (const int k : {pair.a, pair.b})
            for (const QList<double>* lines : {&pieces.at(k).xs, &pieces.at(k).ys})
                for (int i = 1; i < lines->size(); ++i) widest = std::max(widest, lines->at(i) - lines->at(i - 1));
        if (widest > 10 * pair.dz && (unalignedGap <= 0 || widest / pair.dz > unalignedCell / unalignedGap)) {
            unalignedGap = pair.dz;
            unalignedCell = widest;
        }
    }
    if (unalignedGap > 0) {
        const QString w = tr("Metals overlap across %1 µm where their cells cannot be aligned (a slanted side, or neither "
                             "piece a plate of the other), under cells some %2 µm across - %3 times the gap: their "
                             "capacitance there reads low. A largest cell under %4 µm brings it nearer.")
                              .arg(unalignedGap, 0, 'g', 3).arg(unalignedCell, 0, 'g', 3).arg(int(std::round(unalignedCell / unalignedGap)))
                              .arg(10 * unalignedGap, 0, 'g', 3);
        mesh.warnings << w;
        mesh.lines << w;
    }
    if (narrowestFacing > 0 && facingCell > 150 * narrowestFacing) {
        const QString w = tr("Metals face each other across %1 µm, under cells some %2 µm across - %3 times the gap: their "
                             "capacitance reads low, by some 5 % at 250 times. A largest cell under %4 µm brings it within a "
                             "per cent.")
                              .arg(narrowestFacing, 0, 'g', 3).arg(facingCell, 0, 'g', 3).arg(int(std::round(facingCell / narrowestFacing)))
                              .arg(125 * narrowestFacing, 0, 'g', 3);
        mesh.warnings << w;
        mesh.lines << w;
    }
    if (facing > 0)
        mesh.lines << tr("%1 side(s) face another conductor across a gap: their outermost cells are halved until they are no "
                         "wider than a quarter of it, where the charge and the current crowd on both edges.")
                          .arg(facing);

    // Conductors of one layer that touch along an edge are one conductor -
    // the union joins them - and a current crosses from one to the other
    // only where a cell of each shares a whole edge. Where none does (a
    // stretch of slanted edge, or the merge turned off), the solve has them
    // apart, which is said, naming them: it said nothing, and a filter's
    // stub was simply not there.
    if (cellConductor.size() == mesh.cells.size()) {
        QRectF all;
        for (const Conductor& c : conductors) all = all.united(c.outline.boundingRect());
        const double tol = 1e-6 * std::max(1.0, std::max(all.width(), all.height()));
        QHash<QString, QList<int>> byEdge;   // conductors whose cells have that edge
        for (int c = 0; c < mesh.cells.size(); ++c) {
            const QPolygonF& o = mesh.cells.at(c).outline;
            for (int i = 0, n = int(o.size()); i < n; ++i) {
                QList<int>& who = byEdge[mesh.cells.at(c).layer + QLatin1Char('|') + edgeKey(o.at(i), o.at((i + 1) % n))];
                if (!who.contains(cellConductor.at(c))) who << cellConductor.at(c);
            }
        }
        QSet<QPair<int, int>> joined;
        for (const QList<int>& who : std::as_const(byEdge))
            for (int i = 0; i < who.size(); ++i)
                for (int j = i + 1; j < who.size(); ++j) joined.insert(qMakePair(std::min(who.at(i), who.at(j)), std::max(who.at(i), who.at(j))));
        const auto said = [&conductors](int c) {
            const QPointF m = conductors.at(c).outline.boundingRect().center();
            return tr("%1's piece about %2, %3 µm").arg(conductors.at(c).layer).arg(m.x(), 0, 'g', 6).arg(m.y(), 0, 'g', 6);
        };
        for (int a = 0; a < conductors.size(); ++a)
            for (int b = a + 1; b < conductors.size(); ++b) {
                if (conductors.at(a).layer != conductors.at(b).layer || joined.contains(qMakePair(a, b))) continue;
                if (!touchAlongAnEdge(conductors.at(a).outline, conductors.at(b).outline, tol)) continue;
                const QString w = tr("%1 and %2 touch along an edge, but no cell of one shares an edge with a cell of the "
                                     "other, so no current crosses between them here: the solve has them apart. Draw them "
                                     "as one shape, or turn the layer's merge on.")
                                      .arg(said(a), said(b));
                mesh.warnings << w;
                mesh.lines << w;
            }
    }

    // A metal thick beside the gaps it faces: the capacitance across a
    // narrow gap is much of it in the field between the side walls, which
    // neither model holds - a sheet has none, and the two faces' walls carry
    // no charge (rf25_static.h) -, so the answer reads low. On a comb of 1
    // µm metal in 1 µm gaps it was 19 % under Palace, two sheets with no
    // walls giving 0.82 of solid metal; 3 µm in 50 µm gaps, as rf25_fullwave.h
    // validates, is a few per cent. Said where the thickness is a quarter of
    // the narrowest gap or more (B2 of the open items of 8 November: nothing
    // was said).
    QSet<QString> wallsWarned;   // (a sheet's layers said so: their thickness over the ground is not said again, below)
    {
        QMap<QString, double> narrowestGap;   // (µm, by layer: between pieces facing each other across x or y)
        for (int a = 0; a < pieces.size(); ++a)
            for (int b = a + 1; b < pieces.size(); ++b) {
                const QString& layer = conductors.at(pieces.at(a).conductor).layer;
                if (conductors.at(pieces.at(b).conductor).layer != layer) continue;
                const QRectF& A = pieces.at(a).box;
                const QRectF& B = pieces.at(b).box;
                double gap = -1;
                if (B.bottom() > A.top() && B.top() < A.bottom())   // sharing some y: across x
                    gap = std::max(B.left() - A.right(), A.left() - B.right());
                if (B.right() > A.left() && B.left() < A.right()) {   // sharing some x: across y
                    const double g = std::max(B.top() - A.bottom(), A.top() - B.bottom());
                    gap = gap > 0 && g > 0 ? std::min(gap, g) : std::max(gap, g);
                }
                if (gap <= 1e-9 * size) continue;
                // (Two pieces of one conductor with that conductor between
                //  them face no gap, as the grading above has it: two parts of
                //  a dodecagonal spiral's turn with a sliver of a slab between
                //  them were said to be 0.012 µm apart.)
                if (pieces.at(a).conductor == pieces.at(b).conductor
                    && conductors.at(pieces.at(a).conductor)
                           .outline.containsPoint(QPointF(0.5 * (std::max(A.left(), B.left()) + std::min(A.right(), B.right())),
                                                          0.5 * (std::max(A.top(), B.top()) + std::min(A.bottom(), B.bottom()))),
                                                  Qt::OddEvenFill))
                    continue;
                // (A slanted side's part: how far apart the outlines are, as
                //  the grading above has it.)
                if (!pieces.at(a).rectangle || !pieces.at(b).rectangle)
                    gap = std::max(gap, outlineDistance(outlineOfPiece(pieces.at(a)), outlineOfPiece(pieces.at(b))));
                if (!narrowestGap.contains(layer) || gap < narrowestGap.value(layer)) narrowestGap.insert(layer, gap);
            }
        for (const double gap : std::as_const(narrowestGap))
            if (mesh.narrowestGap <= 0 || gap < mesh.narrowestGap) mesh.narrowestGap = gap;
        mesh.gapByLayer = narrowestGap;
        for (auto it = narrowestGap.constBegin(); it != narrowestGap.constEnd(); ++it) {
            const Layer* metal = stack.named(it.key());
            if (metal == nullptr || metal->kind != Layer::Metal || !(metal->thickness > 0)) continue;
            const double t = metal->thickness / perUnit, gap = it.value();
            if (t < kThickBesideGap * gap) continue;
            // (Solved as its two faces - Automatic here is - the walls facing
            //  across a narrow gap hold charge, each edge cell's spread over
            //  its face and its half of the wall by area: still an
            //  approximation of the corners' crowding.)
            const QString w = metal->twoSheets || metal->automatic
                                  ? tr("%1 is %2 µm thick beside gaps as narrow as %3 µm: the field between its side walls "
                                       "is in this answer, the walls facing across them holding charge - each edge cell's "
                                       "spread over its face and its half of the wall, its corners and finger tips taken "
                                       "as straight walls.")
                                        .arg(it.key()).arg(t, 0, 'g', 3).arg(gap, 0, 'g', 3)
                                  : tr("%1 is %2 µm thick beside gaps as narrow as %3 µm: the field between its side walls "
                                       "is not in this answer - a sheet has no walls - so the capacitance across its gaps "
                                       "reads low, by some 20 % where it is as thick as the gap is wide. Solved as its two "
                                       "faces (the metal's Solved as), they are in it.")
                                        .arg(it.key()).arg(t, 0, 'g', 3).arg(gap, 0, 'g', 3);
            if (!metal->twoSheets && !metal->automatic) wallsWarned << it.key();
            mesh.warnings << w;
            mesh.lines << w;
        }
    }
    // A metal thick against its own width: the full wave's inductance at a
    // low frequency reads high, and the more the thicker (a square loop of wire 0.5 by 0.5 mm, 24.2 nH, read 33.4
    // as a sheet). A sheet's field is of no thickness, and its surface
    // impedance adds the inside of a slab on top. As its two faces it holds
    // its section's own inside (facesInside(), P52), except a conductor more
    // than about twice as tall as it is wide, whose faces' field alone holds
    // more than its section's. The loop inductance (rf25_inductance) takes
    // each run as the bar it is.
    //
    // (Which metals are their faces as the solvers decide it, thickened()'s
    // facesDecided(), not the stack's flags: an Automatic metal with no gap
    // beside it is a sheet, which this took for faces and said nothing of -
    // a lone wire 10 µm wide of 8 µm metal read L 27 % high. Of the conductors whole, their mirrors with them.)
    Stack decided = stack;
    const QStringList facedMetals = facesDecided(&decided, conductors, mesh.gapByLayer, nullptr);
    {
        // (Each conductor's width conductorWidthOf()'s: a square pad's its
        //  side, where twice its area over its perimeter, half that, warned
        //  of a 100 µm pad of 10 µm metal as of a 50 µm wire - the review of
        //  54357932.)
        QMap<QString, double> narrowest;
        for (const Conductor& c : conductors) {
            const double width = conductorWidthOf(c.outline);
            if (width > 0 && (!narrowest.contains(c.layer) || width < narrowest.value(c.layer))) narrowest.insert(c.layer, width);
        }
        for (auto it = narrowest.constBegin(); it != narrowest.constEnd(); ++it) {
            const Layer* metal = stack.named(it.key());
            if (metal == nullptr || metal->kind != Layer::Metal || !(metal->thickness > 0)) continue;
            const double t = metal->thickness / perUnit, w = it.value();
            if (t < kThickForInductance * w) continue;
            const bool faces = facedMetals.contains(it.key());
            if (faces && facesInside(w, t) > 0) continue;
            const QString said = faces
                                     ? tr("%1 is %2 µm thick on conductors as narrow as %3 µm: solved as its two faces, a "
                                          "conductor more than about twice as tall as it is wide holds less inside than its two "
                                          "faces' field alone, so the full wave's inductance at a low frequency reads high. "
                                          "rf25_inductance takes it as the bar it is.")
                                           .arg(it.key()).arg(t, 0, 'g', 3).arg(w, 0, 'g', 3)
                                     : tr("%1 is %2 µm thick on conductors as narrow as %3 µm: as a sheet its field is of no "
                                          "thickness, and its surface impedance adds the inside of a slab on top, so the full "
                                          "wave's inductance at a low frequency reads high - some 40 % on a wire as thick as it "
                                          "is wide, by about (mu0/2pi) ln((w + t)/w) a metre and the inside's mu0 t/12 a square. "
                                          "Solved as faces (the metal's Solved as) it is nearer; rf25_inductance takes it as the bar it is.")
                                           .arg(it.key()).arg(t, 0, 'g', 3).arg(w, 0, 'g', 3)
                                         + (metal->automatic ? tr(" (Its Solved as is Automatic, which takes a metal for a sheet where "
                                                                  "no gap narrow beside its thickness faces it.)")
                                                             : QString());
            mesh.warnings << said;
            mesh.lines << said;
        }
    }
    // A sheet thick against how high it stands over the ground: its walls and
    // its top, a thickness higher, are not in a sheet, and Wheeler's
    // correction says how much wider they make it - laminate copper 15 µm
    // thick on 60 µm read its C 7 % under a 3D solve's, with no word (P39 of
    // the open items).
    if (!stack.layers.isEmpty() && stack.layers.first().kind == Layer::Ground) {
        // (Each conductor's width - a rectangle's short side; a trace of any
        //  other shape twice its area over its perimeter, which is its width
        //  where it is long - and its length, its area over that. With their
        //  mirrors, on a plane of symmetry: a strip the plane halves is as
        //  wide as the whole - P36's half warned of strips half as wide as
        //  the whole's, which it did not. And
        //  none that faces another metal across a thin gap, whose charge
        //  is the plates'.)
        //
        // (Which metals the solvers take as their two faces: those asked
        //  to be, and an Automatic one at least a fortieth as thick as the
        //  narrowest gap beside it, as thickened() decides - which the mesh,
        //  of the stack as the model has it, takes for a sheet: warned of
        //  "as a sheet", walls and all, where it was solved as its faces -
        //  the review of d7afd2df.)
        const auto asFaces = [&](const Layer& l) {
            if (l.kind != Layer::Metal || !(l.thickness > 0)) return false;
            if (l.twoSheets) return true;
            const double gap = mesh.gapByLayer.value(l.name, 0);
            return l.automatic && gap > 0 && l.thickness / perUnit >= kAutomaticFaces * gap;
        };
        // (Each metal's faces' heights over the ground, µm, as the solvers
        //  place them: a metal solved as its faces raises those over it by
        //  its thickness, as the plates' heights above do - left out, a
        //  sheet over such a metal was said to stand that much lower.)
        QHash<QString, QPair<double, double>> standing;
        {
            double z = 0;
            for (const Layer& l : stack.layers) {
                if (l.kind == Layer::Dielectric || l.kind == Layer::Substrate) z += l.thickness / perUnit;
                if (l.kind != Layer::Metal) continue;
                const double room = asFaces(l) ? l.thickness / perUnit : 0;
                standing.insert(l.name, qMakePair(z, z + room));
                z += room;
            }
        }
        // (A plate: a conductor overlapping one on another metal by half
        //  itself or more, a tenth of its own height over the ground apart
        //  or less - its charge that metal's, not the ground's. By its own
        //  height, not the cells: by the plates' rule of a tenth of the
        //  cells, a strip 2 µm over another metal was a plate at 50 µm cells
        //  and was warned of at 10 µm - the review of d7afd2df.)
        QSet<int> plates;
        {
            QList<double> areas;
            for (const Conductor& c : conductors) areas << std::abs(areaOf(c.outline));
            for (int i = 0; i < conductors.size(); ++i)
                for (int j = i + 1; j < conductors.size(); ++j) {
                    const Conductor& a = conductors.at(i);
                    const Conductor& b = conductors.at(j);
                    if (a.layer == b.layer || !standing.contains(a.layer) || !standing.contains(b.layer)) continue;
                    if (!a.outline.boundingRect().intersects(b.outline.boundingRect())) continue;
                    const QPair<double, double> sa = standing.value(a.layer), sb = standing.value(b.layer);
                    const double gap = sa.first <= sb.first ? sb.first - sa.second : sa.first - sb.second;
                    if (!(gap > 0)) continue;
                    const bool nearA = gap <= kPlateGap * sa.first, nearB = gap <= kPlateGap * sb.first;
                    if (!nearA && !nearB) continue;
                    const double overlap = std::abs(areaOf(a.outline.intersected(b.outline)));
                    if (nearA && overlap >= 0.5 * areas.at(i)) plates << i;
                    if (nearB && overlap >= 0.5 * areas.at(j)) plates << j;
                }
        }
        struct Trace {
            double width, length;
        };
        QMap<QString, QList<Trace>> tracesOn;
        QMap<QString, double> narrowestOn;   // (each layer's narrowest conductor across, µm)
        for (int k = 0; k < conductors.size(); ++k) {
            const Conductor& c = conductors.at(k);
            const QRectF box = c.outline.boundingRect();
            const double a = std::abs(areaOf(c.outline)), perimeter = perimeterOf(c.outline);
            const bool rectangle = std::abs(a - box.width() * box.height()) <= 1e-9 * box.width() * box.height();
            const double across = rectangle ? std::min(box.width(), box.height())
                                  : perimeter > 0 ? std::min(std::min(box.width(), box.height()), 2 * a / perimeter) : 0;
            if (!(across > 0) || plates.contains(k)) continue;
            if (!narrowestOn.contains(c.layer) || across < narrowestOn.value(c.layer)) narrowestOn.insert(c.layer, across);
            tracesOn[c.layer] << Trace{across, a / across};
        }
        // (A microstrip's impedance of no thickness in one medium,
        //  Hammerstad's, to a constant: its capacitance goes as one over it.
        //  And Wheeler's widening for a thickness, in Hammerstad's two forms:
        //  over its ground's height where it is wide beside it, over its own
        //  width where it is narrow - which the wide one alone made some
        //  eight times too much on a 4 µm line 200 µm up, "93 %", where it is
        //  9 % of its C.)
        const auto impedance = [](double u) {
            return u <= 1 ? 60 * std::log(8 / u + u / 4) : 120 * M_PI / (u + 1.393 + 0.667 * std::log(u + 1.444));
        };
        const auto widening = [](double w, double h, double t) {
            return std::max(0.0, t / M_PI * (1 + std::log(w / h >= 1 / (2 * M_PI) ? 2 * h / t : 4 * M_PI * w / t)));
        };
        for (const Layer& l : stack.layers) {
            if (l.kind != Layer::Metal || asFaces(l) || !(l.thickness > 0) || l.roundWire || !narrowestOn.contains(l.name)) continue;
            const double h = standing.value(l.name).first;   // (what it stands on, µm)
            // (A layer whose walls beside its gaps were said: the same want,
            //  the same remedy - two of each on an inductor's two metals were
            //  four warnings where two say it.)
            if (wallsWarned.contains(l.name)) continue;
            const double t = l.thickness / perUnit;
            if (!(h > 0)) continue;
            // (Twice as thick as it stands high, or more, Wheeler's log no
            //  longer holds - and a sheet is furthest off there: said without
            //  a number - the review of 791a8ed2.)
            if (2 * h <= t) {
                const QString warned = tr("%1 is %2 µm thick as a sheet %3 µm over the ground - thicker than twice its height: its "
                                          "walls and its top are not in this answer, and its capacitance reads well low. Solved as "
                                          "its two faces (the metal's Solved as), they are in it.")
                                           .arg(l.name).arg(t, 0, 'g', 3).arg(h, 0, 'g', 4);
                mesh.warnings << warned;
                mesh.lines << warned;
                continue;
            }
            // (Its conductors' C to the ground, each by its length, as sheets
            //  and widened: laminate copper 15 µm thick on 60 µm, 120 µm wide,
            //  6.9 % - its C was 7 % under a 3D solve's, P39 of the open items;
            //  35 µm copper on 1.6 mm of FR-4, 3 mm wide, 1 %, and on 0.5 mm,
            //  0.96 mm wide, 2.8 %: not said.)
            double asSheets = 0, widened = 0;
            for (const Trace& trace : tracesOn.value(l.name)) {
                asSheets += trace.length / impedance(trace.width / h);
                widened += trace.length / impedance((trace.width + widening(trace.width, h, t)) / h);
            }
            const double more = asSheets > 0 ? widened / asSheets - 1 : 0;
            if (!(more >= 0.05)) continue;
            const QString warned = tr("%1 is %2 µm thick as a sheet %3 µm over the ground: its walls and its top are not in this "
                                      "answer. Wheeler's correction for its thickness, in Hammerstad's microstrip, gives its "
                                      "conductors %4 % more capacitance to the ground than as sheets (its narrowest %5 µm across): "
                                      "it reads low by up to that much, the less the more of their charge other conductors near "
                                      "them hold. Solved as its two faces (the metal's Solved as), they are in it.")
                                       .arg(l.name).arg(t, 0, 'g', 3).arg(h, 0, 'g', 4).arg(100 * more, 0, 'f', 0)
                                       .arg(narrowestOn.value(l.name), 0, 'g', 4);
            mesh.warnings << warned;
            mesh.lines << warned;
        }
    }

    if (axis != 0) {
        // (The half kept: above the plane. A cell across it cut at it.)
        const int whole = int(mesh.cells.size());
        QList<MeshCell> half;
        QList<int> halfConductor;
        for (int k = 0; k < mesh.cells.size(); ++k) {
            MeshCell cell = mesh.cells.at(k);
            const QRectF box = cell.outline.boundingRect();
            const double lo = axis == 1 ? box.left() : box.top(), hi = axis == 1 ? box.right() : box.bottom();
            const double tol = 1e-9 * std::max({1.0, std::abs(lo), std::abs(hi)});
            if (hi <= plane + tol) continue;
            if (lo < plane - tol) {
                const QRectF beyond = axis == 1 ? QRectF(QPointF(plane, box.top() - 1), QPointF(box.right() + 1, box.bottom() + 1))
                                                : QRectF(QPointF(box.left() - 1, plane), QPointF(box.right() + 1, box.bottom() + 1));
                QPolygonF cut = cell.outline.intersected(QPolygonF(beyond));
                if (cut.size() > 1 && cut.first() == cut.last()) cut.removeLast();
                const double a = areaOf(cut);
                if (!(a > 0)) continue;
                cell.outline = cut;
                cell.centre = centroidOf(cut, a);
                cell.area = a;
            }
            half << cell;
            if (k < cellConductor.size()) halfConductor << cellConductor.at(k);
        }
        mesh.cells = half;
        cellConductor = halfConductor;
        mesh.lines << tr("Meshed as the whole with its mirror across the plane of symmetry, %1 cells, and the half of them "
                         "beyond the plane kept: %2.")
                          .arg(whole).arg(half.size());
    }

    // Its cells made to meet side to side, each piece's cells and its
    // neighbour's (conform()).
    conform(mesh.cells, cellConductor);

    // The unknowns: an edge two cells of a layer share is where current
    // flows from one to the other, and is one unknown of a rooftop basis.
    QHash<QString, int> shared;
    for (const MeshCell& cell : std::as_const(mesh.cells))
        for (int i = 0, n = cell.outline.size(); i < n; ++i)
            ++shared[cell.layer + QLatin1Char('|') + edgeKey(cell.outline.at(i), cell.outline.at((i + 1) % n))];
    // A metal solved as its two faces is its cells twice, its edges twice,
    // and a wall up from each cell along its conductor's edge (thickened()):
    // the metals the solvers take so (facesDecided()) - a film a sheet, an
    // Automatic metal its faces where it is thick beside its gaps.
    // (facedMetals above: of the conductors whole, their mirrors with them,
    //  as thickened() is given them on a plane of symmetry.)
    const auto twoFaces = [&facedMetals](const QString& layer) { return facedMetals.contains(layer); };
    int unknowns = 0, charges = int(mesh.cells.size());
    for (auto it = shared.constBegin(); it != shared.constEnd(); ++it)
        if (it.value() >= 2) unknowns += twoFaces(it.key().section(QLatin1Char('|'), 0, 0)) ? 2 : 1;
    QStringList faced;
    for (const MeshCell& cell : std::as_const(mesh.cells))
        if (twoFaces(cell.layer)) {
            ++charges;
            if (cell.onEdge) ++unknowns;
            if (!faced.contains(cell.layer)) faced << cell.layer;
        }
    mesh.charges = charges;
    for (const MeshCell& cell : std::as_const(mesh.cells)) {
        const QRectF box = cell.outline.boundingRect();
        const double shorter = std::min(box.width(), box.height()), longer = std::max(box.width(), box.height());
        if (shorter > 0 && (mesh.smallest <= 0 || shorter < mesh.smallest)) mesh.smallest = shorter;
        mesh.largest = std::max(mesh.largest, longer);
    }
    mesh.cost = how.compressed ? costOf(unknowns + charges, true) : costOf(unknowns);
    // (For a full-wave solve held compressed, its cells and runs together
    //  are its unknowns, and the charge solve's dense matrix is no one's.)
    if (how.compressed)
        mesh.lines << tr("%1 cell(s) on %2 layer(s), and the %3 edge(s) between them: a full-wave solve's %4 unknowns, %5.")
                          .arg(charges).arg(mesh.byLayer().size()).arg(unknowns).arg(unknowns + charges)
                          .arg(costOf(unknowns + charges, true).said.section(QLatin1String("; "), 1));
    else
        mesh.lines << tr("%1 cell(s) on %2 layer(s): %3. The %4 edge(s) between them are a full-wave solve's unknowns: %5.")
                          .arg(charges).arg(mesh.byLayer().size()).arg(chargeCostOf(charges, how.chargeCompressed))
                          .arg(unknowns).arg(mesh.cost.said);
    if (!faced.isEmpty())
        mesh.lines << tr("%1 solved as its two faces: its cells and their edges twice, and a wall up from each cell along "
                         "its edges, in those counts.")
                          .arg(faced.join(QStringLiteral(", ")));
    if (mesh.cost.tooBig)
        mesh.problems << mesh.cost.said;
    // (The mesh's own cells: a metal's top face is the solver's to refuse,
    //  which a calibration strip too long for it is cut short for.)
    if (!how.compressed && mesh.cells.size() > (how.chargeCompressed ? kMostCompressedCells : kMostCharges))
        mesh.problems << chargeCostOf(int(mesh.cells.size()), how.chargeCompressed);
    if (mesh.cells.isEmpty()) mesh.problems << tr("The mesh came out empty.");
    return mesh;
}


bool Thickened::made(const QString& layer) const
{
    const Layer* l = stack.named(layer);
    return l != nullptr && (l->face == Layer::Top || l->face == Layer::Walls);
}

Thickened thickened(const Stack& given, const QList<Conductor>& conductors, const Mesh& mesh, const QList<Conductor>* whole)
{
    Thickened out;
    // (Which metals are their two faces: facesDecided(), as the mesh counts
    //  its charges by - of the whole on a plane of symmetry: a 1.5 µm metal
    //  on a 100 µm line was a film whole and two faces in its half, Z21 0.85
    //  % apart and twice the charges.)
    Stack stack = given;
    const QStringList metals = facesDecided(&stack, whole != nullptr ? *whole : conductors, mesh.gapByLayer, &out.said);
    out.stack = stack;
    out.conductors = conductors;
    out.mesh = mesh;
    out.cells = int(mesh.cells.size());
    if (metals.isEmpty()) return out;
    out.any = true;

    // A name no layer of the stack has, for what is made.
    const auto unused = [&stack](const QString& wanted) {
        QString name = wanted;
        for (int k = 2; stack.named(name) != nullptr; ++k) name = QStringLiteral("%1 %2").arg(wanted).arg(k);
        return name;
    };
    QHash<QString, QString> topOf, wallsOf;   // (by the metal's name)
    QList<Layer> layers;
    for (int i = 0; i < stack.layers.size(); ++i) {
        Layer metal = stack.layers.at(i);
        if (!metals.contains(metal.name)) {
            layers << metal;
            continue;
        }
        metal.face = Layer::Bottom;
        metal.faceOf = metal.name;
        layers << metal;
        // The room its thickness takes, filled with what is over it: the
        // first dielectric above it short of another metal, else the air.
        Layer room;
        room.kind = Layer::Dielectric;
        room.name = unused(tr("%1 (beside)").arg(metal.name));
        room.thickness = metal.thickness;
        room.z0 = metal.z0;
        room.z1 = metal.z1;
        QString filled = tr("the air");
        for (int j = i + 1; j < stack.layers.size(); ++j) {
            const Layer& above = stack.layers.at(j);
            if (above.kind == Layer::Metal || above.kind == Layer::Cover) break;
            if (above.kind != Layer::Dielectric) continue;
            room.epsr = above.epsr;
            room.tand = above.tand;
            filled = above.name;
            break;
        }
        room.faceOf = metal.name;
        layers << room;
        Layer top = metal;
        top.tag.clear();
        top.name = unused(tr("%1 (top)").arg(metal.name));
        top.face = Layer::Top;
        top.z0 = top.z1 = metal.z1;
        layers << top;
        Layer walls;
        walls.kind = Layer::Via;
        walls.name = unused(tr("%1 (walls)").arg(metal.name));
        walls.from = metal.name;
        walls.to = top.name;
        walls.sigma = metal.sigma;
        walls.face = Layer::Walls;
        walls.faceOf = metal.name;
        walls.z0 = metal.z0;
        walls.z1 = metal.z1;
        layers << walls;
        topOf.insert(metal.name, top.name);
        wallsOf.insert(metal.name, walls.name);
        out.said << tr("%1 is solved as its two faces: a sheet at its bottom and one at its top, %2 µm over it, with %3 "
                       "between them beside its walls, which join the two along its edges.")
                        .arg(metal.name).arg(metal.thickness * 1e6, 0, 'g', 4).arg(filled);
    }
    // A via up from such a metal leaves its top face; one down from it, or
    // up to it from below, its bottom. (Above by its place in the stack,
    // which is its place in z: a stack made by hand has no z of its own.)
    const auto place = [&stack](const Layer* l) { return l == nullptr ? -1 : int(l - stack.layers.constData()); };
    for (Layer& via : layers) {
        if (via.kind != Layer::Via || via.face != Layer::Whole) continue;
        const Layer* from = stack.named(via.from);
        const Layer* to = stack.named(via.to);
        const auto upper = [&stack, &place](const Layer* a, const Layer* b, const QString& bName) {
            return a != nullptr && b != nullptr && !stack.isGround(bName) && place(b) > place(a);
        };
        if (from != nullptr && topOf.contains(from->name) && upper(from, to, via.to)) via.from = topOf.value(from->name);
        else if (to != nullptr && topOf.contains(to->name) && upper(to, from, via.from)) via.to = topOf.value(to->name);
    }
    out.stack.layers = layers;

    // The shapes: each on its top face too, and its walls the shape over
    // both, which is what joins the two into one conductor.
    for (const Conductor& c : conductors) {
        const Layer* metal = stack.named(c.layer);
        if (metal == nullptr || !topOf.contains(metal->name)) continue;
        Conductor top = c;
        top.layer = topOf.value(metal->name);
        out.conductors << top;
        Conductor walls = c;
        walls.layer = wallsOf.value(metal->name);
        out.conductors << walls;
    }

    // The cells: each of a two-sheet metal's on its top face too, and a
    // wall along each side that is its conductor's edge - a strip a tenth
    // of the cell deep inside that side, which lands on the cell and on its
    // top. Its outline is only where it stands: the walls carry the
    // current between the faces, and their own impedance is nothing beside
    // the rest.
    for (int c = 0; c < out.cells; ++c) {
        const MeshCell& cell = mesh.cells.at(c);
        const Layer* metal = stack.named(cell.layer);
        if (metal == nullptr || !topOf.contains(metal->name)) continue;
        MeshCell top = cell;
        top.layer = topOf.value(metal->name);
        out.topOf.insert(c, int(out.mesh.cells.size()));
        out.bottomOf.insert(int(out.mesh.cells.size()), c);
        out.mesh.cells << top;
    }
    for (int c = 0; c < out.cells; ++c) {
        const MeshCell& cell = mesh.cells.at(c);
        const Layer* metal = stack.named(cell.layer);
        if (metal == nullptr || !topOf.contains(metal->name) || !cell.onEdge) continue;
        const QPolygonF& outline = cell.outline;
        const QRectF box = outline.boundingRect();
        const double size = std::max(box.width(), box.height());
        for (int i = 0, n = int(outline.size()); i < n; ++i) {
            const QPointF a = outline.at(i), b = outline.at((i + 1) % n), d = b - a;
            const double length = std::hypot(d.x(), d.y());
            if (!(length > 0)) continue;
            QPointF outward(d.y() / length, -d.x() / length);
            const QPointF middle = (a + b) / 2;
            if (outline.containsPoint(middle + outward * (1e-3 * length), Qt::OddEvenFill)) outward = -outward;
            const QPointF probe = middle + outward * (1e-4 * size);
            // (Of the conductors whole on a plane of symmetry: the cut along
            //  it is no edge, and walls stood along it - a 50 µm metal's
            //  half 8e-4 off the whole in its series reactance.)
            bool covered = false;
            for (const Conductor& shape : whole != nullptr ? *whole : conductors)
                if (shape.layer == cell.layer && shape.outline.boundingRect().contains(probe)
                    && shape.outline.containsPoint(probe, Qt::OddEvenFill)) {
                    covered = true;
                    break;
                }
            if (covered) continue;
            double deep = 0;   // (how far the cell reaches in from that side)
            for (const QPointF& p : outline) deep = std::max(deep, -((p - a).x() * outward.x() + (p - a).y() * outward.y()));
            const QPointF in = -outward * (0.1 * deep);
            MeshCell wall;
            wall.layer = wallsOf.value(metal->name);
            wall.outline = QPolygonF({a, b, b + in, a + in});
            wall.centre = middle + in / 2;
            wall.area = length * 0.1 * deep;
            wall.onEdge = true;
            out.wallOn.insert(int(out.mesh.cells.size()), c);
            out.mesh.cells << wall;
            ++out.walls;
        }
    }
    return out;
}


double wallExcess(double tau)
{
    // (t / S, and the excess capacitance per length over eps: the 2D
    //  solve's at S/32 and S/64, extrapolated linearly in the grid.)
    static const double points[][2] = {{0.0625, 0.0080}, {0.125, 0.0203}, {0.25, 0.0492}, {0.5, 0.1258},
                                       {1.0, 0.3489},    {2.0, 0.9806},   {4.0, 2.60}};
    constexpr int n = int(sizeof(points) / sizeof(points[0]));
    if (!(tau >= 1.0 / 32)) return 0;
    if (tau <= points[0][0]) return points[0][1] * std::pow(tau / points[0][0], 2);   // (as tau squared, in the limit)
    if (tau >= points[n - 1][0])
        return points[n - 1][1] + (tau - points[n - 1][0]) * (points[n - 1][1] - points[n - 2][1]) / (points[n - 1][0] - points[n - 2][0]);
    int i = 0;
    while (points[i + 1][0] < tau) ++i;
    const double f = std::log(tau / points[i][0]) / std::log(points[i + 1][0] / points[i][0]);
    return std::exp(std::log(points[i][1]) + f * (std::log(points[i + 1][1]) - std::log(points[i][1])));
}

QList<WallGap> wallGapsOf(const Thickened& thick, double micrometre)
{
    QList<WallGap> out;
    if (!thick.any || thick.wallOn.isEmpty()) return out;
    const double perUnit = micrometre > 0 ? micrometre : 1e-6;
    // Each wall: its metal, its side's line and extent, and which way it
    // faces (0 +x, 1 -x, 2 +y, 3 -y).
    struct Wall {
        int cell, on;
        QString metal;
        int facing;
        double line, lo, hi;
    };
    QHash<QString, QList<Wall>> byMetal;
    for (auto it = thick.wallOn.constBegin(); it != thick.wallOn.constEnd(); ++it) {
        const MeshCell& wall = thick.mesh.cells.at(it.key());
        if (wall.outline.size() < 4) continue;
        const QPointF a = wall.outline.at(0), b = wall.outline.at(1), in = wall.outline.at(2) - wall.outline.at(1);
        const QPointF d = b - a;
        const double len = std::hypot(d.x(), d.y());
        if (!(len > 0)) continue;
        const Layer* layer = thick.stack.named(wall.layer);
        if (layer == nullptr) continue;
        Wall w{it.key(), it.value(), layer->faceOf, -1, 0, 0, 0};
        if (std::abs(d.y()) <= 1e-9 * len) {   // along x: facing +y or -y
            w.facing = in.y() < 0 ? 2 : 3;
            w.line = a.y();
            w.lo = std::min(a.x(), b.x());
            w.hi = std::max(a.x(), b.x());
        } else if (std::abs(d.x()) <= 1e-9 * len) {   // along y: facing +x or -x
            w.facing = in.x() < 0 ? 0 : 1;
            w.line = a.x();
            w.lo = std::min(a.y(), b.y());
            w.hi = std::max(a.y(), b.y());
        } else {
            continue;   // (a slanted wall: none)
        }
        byMetal[w.metal] << w;
    }
    for (auto it = byMetal.constBegin(); it != byMetal.constEnd(); ++it) {
        const Layer* metal = thick.stack.named(it.key());
        if (metal == nullptr || !(metal->thickness > 0)) continue;
        const double t = metal->thickness / perUnit;   // mesh units
        // What fills the room beside the walls: the layer made for it.
        double epsr = 1;
        for (const Layer& l : thick.stack.layers)
            if (l.kind == Layer::Dielectric && l.faceOf == it.key()) epsr = l.epsr;
        const QList<Wall>& walls = it.value();
        for (const int axis : {0, 2}) {
            // Walls facing + on this axis, each with the nearest facing -.
            QList<const Wall*> plus, minus;
            for (const Wall& w : walls) {
                if (w.facing == axis) plus << &w;
                if (w.facing == axis + 1) minus << &w;
            }
            std::sort(minus.begin(), minus.end(), [](const Wall* x, const Wall* y) { return x->line < y->line; });
            for (const Wall* p : std::as_const(plus)) {
                const auto first = std::lower_bound(minus.begin(), minus.end(), p->line + 1e-9 * t,
                                                    [](const Wall* w, double v) { return w->line < v; });
                double nearest = -1;
                for (auto m = first; m != minus.end() && (*m)->line - p->line <= 16 * t; ++m) {
                    if (nearest >= 0 && (*m)->line > nearest + 1e-9 * t) break;
                    const double overlap = std::min(p->hi, (*m)->hi) - std::max(p->lo, (*m)->lo);
                    if (!(overlap > 1e-9 * t)) continue;
                    nearest = (*m)->line;
                    const double gap = (*m)->line - p->line;
                    const double excess = wallExcess(t / gap);
                    if (!(excess > 0)) continue;
                    WallGap g;
                    g.a = p->on;
                    g.b = (*m)->on;
                    g.gap = gap * perUnit;
                    g.length = overlap * perUnit;
                    g.capacitance = 8.8541878128e-12 * epsr * excess * g.length;   // (eps0)
                    g.epsr = epsr;
                    g.alongX = axis == 2;
                    g.lo = std::max(p->lo, (*m)->lo);
                    g.hi = std::min(p->hi, (*m)->hi);
                    out << g;
                }
            }
        }
    }
    return out;
}

} // namespace qucs_s::rf25
