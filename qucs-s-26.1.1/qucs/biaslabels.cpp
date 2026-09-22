/*
 * biaslabels.cpp - where the DC bias labels of a schematic go: beside their
 * node, clear of the symbols, texts, wires and of one another
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "biaslabels.h"

#include <QLineF>
#include <QRectF>

#include <algorithm>
#include <numeric>

namespace qucs_s::bias {

namespace {

constexpr int kNear = 4;     // the gap between a node and a label beside it
constexpr int kFar = 24;     // ... and a label a step further out
constexpr int kNearCandidates = 8;

// What each kind of cover costs, per unit of area (a wire: per unit of
// length times the label's height, so that a wire right across the label
// costs its area). A label a step away from its node costs half its area,
// so that it only goes there when what it would cover beside the node
// costs more. A label over another one hides a value, which covering a
// symbol does not: place() puts that before any of these, and cost()
// weighs it by kLabelWeight.
constexpr double kLabelWeight = 8.0;
constexpr double kBoxWeight = 2.0;
constexpr double kWireWeight = 1.0;
constexpr double kFarPenalty = 0.5;

double area(const QRect& r) { return r.isEmpty() ? 0.0 : double(r.width()) * double(r.height()); }

// The length of a segment inside a rectangle (Liang-Barsky).
double clippedLength(const QLine& line, const QRectF& r)
{
    const QLineF l(line);
    double t0 = 0.0, t1 = 1.0;
    const double dx = l.dx(), dy = l.dy();
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {l.x1() - r.left(), r.right() - l.x1(), l.y1() - r.top(), r.bottom() - l.y1()};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0) {
            // Parallel to this edge: outside it, or along it (a wire along
            // the edge of a label does it no harm).
            if (q[i] <= 0.0) return 0.0;
            continue;
        }
        const double t = q[i] / p[i];
        if (p[i] < 0.0) t0 = std::max(t0, t);
        else t1 = std::min(t1, t);
        if (t0 > t1) return 0.0;
    }
    return (t1 - t0) * l.length();
}

double staticCost(const QRect& box, const Obstacles& obstacles)
{
    double c = 0.0;
    for (const QRect& b : obstacles.boxes) c += kBoxWeight * area(box.intersected(b));
    for (const QLine& w : obstacles.wires) c += kWireWeight * clippedLength(w, QRectF(box)) * box.height();
    return c;
}

} // namespace

QList<QRect> candidates(const Label& label)
{
    const int w = label.size.width(), h = label.size.height();
    const int x = label.anchor.x(), y = label.anchor.y();
    QList<QRect> out;
    for (const int gap : {kNear, kFar}) {
        const QRect ul(x - gap - w, y - gap - h, w, h), ur(x + gap, y - gap - h, w, h);
        const QRect ll(x - gap - w, y + gap, w, h), lr(x + gap, y + gap, w, h);
        const QRect n(x - w / 2, y - gap - h, w, h), s(x - w / 2, y + gap, w, h);
        const QRect west(x - gap - w, y - h / 2, w, h), east(x + gap, y - h / 2, w, h);
        if (label.current) out << ur << ul << lr << ll << n << s << east << west;
        else out << ul << ur << ll << lr << n << s << west << east;
    }
    return out;
}

double cost(const QRect& box, const Obstacles& obstacles, const QList<QRect>& placed)
{
    double c = staticCost(box, obstacles);
    for (const QRect& p : placed) c += kLabelWeight * area(box.intersected(p));
    return c;
}

QList<Placement> place(const QList<Label>& labels, const Obstacles& obstacles)
{
    // The candidates of each label and what the schematic alone makes of
    // them; the labels with the fewest free places beside their node are
    // placed first, while there is most room.
    QList<QList<QRect>> boxes;
    QList<QList<double>> fixed;
    QList<int> freeNear;
    for (const Label& l : labels) {
        const QList<QRect> c = candidates(l);
        QList<double> costs;
        int free = 0;
        for (int i = 0; i < c.size(); ++i) {
            costs << staticCost(c.at(i), obstacles) + (i < kNearCandidates ? 0.0 : kFarPenalty * area(c.at(i)));
            if (i < kNearCandidates && costs.last() == 0.0) ++free;
        }
        boxes << c;
        fixed << costs;
        freeNear << free;
    }
    QList<int> order(labels.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&freeNear](int a, int b) { return freeNear.at(a) < freeNear.at(b); });

    QList<Placement> result(labels.size());
    QList<QRect> placed;
    for (const int i : order) {
        // The least of the labels placed already covered, then the least
        // of the rest; the earlier candidate when that is equal.
        int best = 0;
        double bestLabels = 0.0, bestRest = 0.0;
        for (int k = 0; k < boxes.at(i).size(); ++k) {
            double covered = 0.0;
            for (const QRect& p : placed) covered += area(boxes.at(i).at(k).intersected(p));
            const double rest = fixed.at(i).at(k);
            if (k == 0 || covered < bestLabels || (covered == bestLabels && rest < bestRest)) {
                best = k;
                bestLabels = covered;
                bestRest = rest;
            }
        }
        const QRect box = boxes.at(i).at(best);
        const bool far = best >= kNearCandidates;
        result[i] = Placement{box, far, kLabelWeight * bestLabels + bestRest - (far ? kFarPenalty * area(box) : 0.0)};
        placed << box;
    }
    return result;
}

} // namespace qucs_s::bias
