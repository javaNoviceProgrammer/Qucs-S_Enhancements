/*
 * speclimits.cpp - a diagram's spec limits: upper and lower lines, levels and
 * piecewise masks its traces must keep within; where they do not, and the
 * verdict
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "speclimits.h"

#include "diagram.h"
#include "graph.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace qucs_s::limits {

namespace {

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

// x where a margin goes from \a m0 (at \a x0) to \a m1 (at \a x1) through 0.
double crossing(double x0, double m0, double x1, double m1)
{
    if (!std::isfinite(x0) || !std::isfinite(m0) || m1 == m0) return x1;
    return x0 + (x1 - x0) * (0.0 - m0) / (m1 - m0);
}

} // namespace

double Limit::at(double x, bool logX, bool logY) const
{
    if (points.isEmpty() || !std::isfinite(x)) return NaN;
    if (points.size() == 1) return points.first().y();   // a level
    if (x < points.first().x() || x > points.last().x()) return NaN;
    // At a step (two points of one x), the stricter of the two.
    double found = NaN;
    for (int i = 0; i + 1 < points.size(); ++i) {
        const QPointF a = points.at(i), b = points.at(i + 1);
        if (x < a.x() || x > b.x()) continue;
        double y;
        if (b.x() == a.x()) {
            y = side == Upper ? std::min(a.y(), b.y()) : std::max(a.y(), b.y());
        } else {
            const bool lx = logX && a.x() > 0 && b.x() > 0 && x > 0;
            const double t = lx ? std::log(x / a.x()) / std::log(b.x() / a.x()) : (x - a.x()) / (b.x() - a.x());
            const bool ly = logY && a.y() > 0 && b.y() > 0;
            y = ly ? a.y() * std::pow(b.y() / a.y(), t) : a.y() + (b.y() - a.y()) * t;
        }
        if (std::isnan(found)) found = y;
        else found = side == Upper ? std::min(found, y) : std::max(found, y);
    }
    return found;
}

double Limit::from() const
{
    return points.size() >= 2 ? points.first().x() : -std::numeric_limits<double>::infinity();
}

double Limit::to() const
{
    return points.size() >= 2 ? points.last().x() : std::numeric_limits<double>::infinity();
}

QString Limit::save() const
{
    QStringList xy;
    for (const QPointF& p : points) xy << QString::number(p.x(), 'g', 15) + QLatin1Char(',') + QString::number(p.y(), 'g', 15);
    QString text = label;
    text.replace(QLatin1Char('"'), QLatin1Char('\''));
    return QStringLiteral("<Limit %1 %2 %3 \"%4\" %5>").arg(int(side)).arg(axis).arg(pane).arg(text, xy.join(QLatin1Char(';')));
}

bool Limit::load(const QString& line, Limit* limit)
{
    static const QRegularExpression form(QStringLiteral("^<Limit (\\d) (\\d) (-?\\d+) \"([^\"]*)\" ([^>]*)>$"));
    const QRegularExpressionMatch m = form.match(line.trimmed());
    if (!m.hasMatch()) return false;
    Limit l;
    l.side = m.captured(1) == QLatin1String("1") ? Lower : Upper;
    l.axis = m.captured(2) == QLatin1String("1") ? 1 : 0;
    l.pane = std::clamp(m.captured(3).toInt(), 0, 63);
    l.label = m.captured(4);
    for (const QString& pair : m.captured(5).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        bool okx = false, oky = false;
        const double x = pair.section(QLatin1Char(','), 0, 0).toDouble(&okx);
        const double y = pair.section(QLatin1Char(','), 1, 1).toDouble(&oky);
        if (!okx || !oky || !std::isfinite(x) || !std::isfinite(y)) return false;
        l.points << QPointF(x, y);
    }
    if (l.points.isEmpty()) return false;
    for (int i = 0; i + 1 < l.points.size(); ++i)
        if (l.points.at(i + 1).x() < l.points.at(i).x()) return false;
    *limit = l;
    return true;
}

QList<int> unchecked(const Diagram* d)
{
    QList<int> out;
    for (int li = 0; li < d->limits.size(); ++li) {
        const Axis* axis = d->limitAxis(d->limits.at(li));
        bool checked = false;
        for (const Graph* g : d->Graphs) checked = checked || (axis != nullptr && g->cPointsY != nullptr && d->graphAxis(g) == axis);
        if (!checked) out << li;
    }
    return out;
}

QString sideName(Limit::Side side)
{
    return side == Limit::Lower ? QStringLiteral("lower") : QStringLiteral("upper");
}

void curveOf(const Graph* g, int curve, const Axis* axis, QVector<double>* x, QVector<double>* y)
{
    x->clear();
    y->clear();
    const DataX* xs = g->axis(0);
    if (xs == nullptr || xs->Points == nullptr || g->cPointsY == nullptr || curve < 0 || curve >= g->countY) return;
    const int count = xs->count;
    x->reserve(count);
    y->reserve(count);
    const bool decibels = axis && axis->log && axis->Units != Axis::NoUnits;
    for (int i = 0; i < count; ++i) {
        const double re = g->cPointsY[2 * (curve * count + i)], im = g->cPointsY[2 * (curve * count + i) + 1];
        double v = std::fabs(im) > 1e-250 ? std::hypot(re, im) : re;
        if (decibels) v = qucs::num2db(std::fabs(v), axis->Units);
        x->append(xs->Points[i]);
        y->append(v);
    }
}

QList<Violation> check(const Diagram* d)
{
    QList<Violation> out;
    for (int li = 0; li < d->limits.size(); ++li) {
        const Limit& limit = d->limits.at(li);
        const Axis* axis = d->limitAxis(limit);
        // (Straight as drawn: on a logarithmic axis, in the logarithm.)
        const bool logX = d->xAxis.log;
        const bool logY = axis && axis->log && axis->Units == Axis::NoUnits;
        for (int gi = 0; gi < d->Graphs.size(); ++gi) {
            const Graph* g = d->Graphs.at(gi);
            if (axis == nullptr || d->graphAxis(g) != axis || g->cPointsY == nullptr) continue;
            // Each curve's stretches beyond it, with the worst in each.
            QList<Violation> found;
            QVector<double> xs, ys;
            for (int c = 0; c < g->countY; ++c) {
                curveOf(g, c, axis, &xs, &ys);
                // Where the margin is taken: each sample, and each of the
                // limit's points within the curve (the curve straight
                // between its samples there) - a step's both sides, its
                // value coming in then going on: so a stretch begins and
                // ends where the trace meets the limit, a step's edge too.
                struct At {
                    double x, y, lim;
                    int rank;   // at one x: a step's value coming in (0), the sample (1), going on (2)
                };
                QVector<At> at;
                at.reserve(xs.size() + limit.points.size() * 2);
                for (int i = 0; i < xs.size(); ++i) at.append({xs.at(i), ys.at(i), limit.at(xs.at(i), logX, logY), 1});
                const bool rising = xs.size() >= 2 && xs.first() < xs.last();
                if (rising && limit.points.size() >= 2) {
                    const double near = 1e-9 * (xs.last() - xs.first());   // (a sample there, within rounding)
                    for (int k = 0; k < limit.points.size(); ++k) {
                        const double vx = limit.points.at(k).x();
                        if (vx < xs.first() - near || vx > xs.last() + near) continue;
                        if (k > 0 && limit.points.at(k - 1).x() == vx) continue;   // (a step: once, below)
                        int last = k;
                        while (last + 1 < limit.points.size() && limit.points.at(last + 1).x() == vx) ++last;
                        const bool step = last != k;
                        const int j = int(std::upper_bound(xs.cbegin(), xs.cend(), vx) - xs.cbegin());
                        const bool before = j > 0 && std::abs(xs.at(j - 1) - vx) <= near;
                        const bool after = j < xs.size() && std::abs(xs.at(j) - vx) <= near;
                        double x = vx, y;
                        if (before || after) {
                            if (!step) continue;   // the sample there tells it
                            const int i = before ? j - 1 : j;
                            x = xs.at(i);
                            y = ys.at(i);
                        } else {
                            if (j <= 0 || j >= xs.size()) continue;
                            y = ys.at(j - 1) + (ys.at(j) - ys.at(j - 1)) * (vx - xs.at(j - 1)) / (xs.at(j) - xs.at(j - 1));
                        }
                        // The values the limit has there: coming in, going on.
                        at.append({x, y, limit.points.at(k).y(), 0});
                        at.append({x, y, limit.points.at(last).y(), 2});
                    }
                    std::stable_sort(at.begin(), at.end(), [](const At& a, const At& b) { return a.x < b.x || (a.x == b.x && a.rank < b.rank); });
                }
                double prevX = NaN, prevM = NaN;
                bool open = false;
                Violation v{gi, li};
                for (const At& p : std::as_const(at)) {
                    if (!std::isfinite(p.lim) || !std::isfinite(p.y)) {
                        if (open) {
                            v.to = prevX;
                            found << v;
                            open = false;
                        }
                        prevX = prevM = NaN;
                        continue;
                    }
                    const double m = limit.side == Limit::Upper ? p.y - p.lim : p.lim - p.y;
                    if (m > 0) {
                        if (!open) {
                            v = Violation{gi, li};
                            v.from = std::isfinite(prevM) ? crossing(prevX, prevM, p.x, m) : p.x;
                            v.worst = m;
                            v.worstAt = p.x;
                            open = true;
                        } else if (m > v.worst) {
                            v.worst = m;
                            v.worstAt = p.x;
                        }
                    } else if (open) {
                        v.to = crossing(prevX, prevM, p.x, m);
                        found << v;
                        open = false;
                    }
                    prevX = p.x;
                    prevM = m;
                }
                if (open) {
                    v.to = prevX;
                    found << v;
                }
            }
            // The curves' stretches that overlap, as one.
            std::sort(found.begin(), found.end(), [](const Violation& a, const Violation& b) { return a.from < b.from; });
            for (const Violation& v : std::as_const(found)) {
                if (!out.isEmpty() && out.last().graph == gi && out.last().limit == li && v.from <= out.last().to) {
                    Violation& last = out.last();
                    last.to = std::max(last.to, v.to);
                    if (v.worst > last.worst) {
                        last.worst = v.worst;
                        last.worstAt = v.worstAt;
                    }
                } else {
                    out << v;
                }
            }
        }
    }
    return out;
}

} // namespace qucs_s::limits
