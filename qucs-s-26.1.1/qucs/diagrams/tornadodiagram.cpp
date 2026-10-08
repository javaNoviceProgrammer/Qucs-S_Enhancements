/*
 * tornadodiagram.cpp - the tornado chart: a bar for each trace, sorted by
 * size - a sensitivity run's parts, corner or Monte Carlo spreads
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "tornadodiagram.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace {

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

QString fieldText(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 15) : QStringLiteral("-");
}

double fieldValue(const QString& s)
{
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok && std::isfinite(v) ? v : NaN;
}

QString bare(const QString& var)
{
    return var.mid(var.lastIndexOf(QLatin1Char('/')) + 1);
}

// The bars' colours: up from 0, down from 0 (a spread's the first).
const QColor kUp(72, 120, 208);
const QColor kDown(238, 133, 74);

} // namespace

TornadoDiagram::TornadoDiagram(int cx, int cy) : RectDiagram(cx, cy), at(NaN)
{
    x2 = 320;
    y2 = 240;
    x3 = x2 + 7;
    // (Not "Tornado": a name with a T first is a table's, a timing
    // diagram's or a truth table's to the dialog and the mouse.)
    Name = "Bars";
    calcDiagram();
}

Diagram* TornadoDiagram::newOne()
{
    return new TornadoDiagram();
}

Element* TornadoDiagram::info(QString& Name, char*& BitmapFile, bool getNewOne)
{
    Name = QObject::tr("Tornado Chart");
    BitmapFile = (char*)"tornado";
    if (getNewOne) return new TornadoDiagram();
    return nullptr;
}

QStringList TornadoDiagram::sensitivityParts(const QStringList& names, const std::function<bool(const QString&)>& nonzero)
{
    const QSet<QString> all(names.cbegin(), names.cend());
    // A part's own entry has no "_" ("r1", "v1") and its parameters'
    // beside it ("r1_m", "v1_freq"): another run's variables are none.
    QSet<QString> withParameters;
    for (const QString& name : names)
        if (name.contains(QLatin1Char('_'))) withParameters.insert(name.section(QLatin1Char('_'), 0, 0));
    QStringList parts;
    for (const QString& name : names) {
        if (name.contains(QLatin1Char('_')) || !withParameters.contains(name)) continue;
        QString pick = all.contains(name + QStringLiteral("_scale")) ? name + QStringLiteral("_scale") : name;
        if (nonzero && !nonzero(pick)) {
            if (pick != name && nonzero(name)) pick = name;
            else continue;
        }
        parts << pick;
    }
    return parts;
}

void TornadoDiagram::collect()
{
    m_bars.clear();
    m_nothing = m_smaller = 0;
    m_noData.clear();
    QList<Bar> all;
    for (int k = 0; k < Graphs.size(); ++k) {
        const Graph* g = Graphs.at(k);
        const DataX* xs = g->axis(0);
        if (g->cPointsY == nullptr || xs == nullptr || xs->Points == nullptr || xs->count < 1) {
            m_noData << bare(g->Var);
            continue;
        }
        const auto valueOf = [&](qint64 i) {
            const double re = g->cPointsY[2 * i], im = g->cPointsY[2 * i + 1];
            return std::fabs(im) > 1e-250 ? std::hypot(re, im) : re;
        };
        // The point: the sample nearest 'at' on its first curve.
        int point = 0;
        if (std::isfinite(at))
            for (int i = 1; i < xs->count; ++i)
                if (std::abs(xs->Points[i] - at) < std::abs(xs->Points[point] - at)) point = i;
        Bar b;
        b.name = bare(g->Var);
        b.graph = k;
        b.value = valueOf(point);
        if (!std::isfinite(b.value)) {
            m_noData << b.name;
            continue;
        }
        if (mode == Spread) {
            b.low = b.high = b.value;
            const qint64 n = qint64(xs->count) * std::max(1, g->countY);
            for (qint64 i = 0; i < n; ++i) {
                const double v = valueOf(i);
                if (!std::isfinite(v)) continue;
                b.low = std::min(b.low, v);
                b.high = std::max(b.high, v);
            }
        } else {
            b.low = std::min(0.0, b.value);
            b.high = std::max(0.0, b.value);
        }
        all << b;
    }
    // Nothing: 0, or below a millionth of a millionth of the largest.
    double largest = 0;
    for (const Bar& b : std::as_const(all)) largest = std::max(largest, b.size());
    for (const Bar& b : std::as_const(all)) {
        if (!(b.size() > 1e-12 * largest)) ++m_nothing;
        else m_bars << b;
    }
    std::stable_sort(m_bars.begin(), m_bars.end(), [](const Bar& a, const Bar& b) { return a.size() > b.size(); });
    if (m_bars.size() > bars) {
        m_smaller = int(m_bars.size()) - bars;
        m_bars = m_bars.mid(0, bars);
    }
}

void TornadoDiagram::getAxisLimits(Graph*)
{
    // (Its range is the bars', laid out in calcDiagram(); no y axis: a row
    // a bar.)
}

int TornadoDiagram::calcDiagram()
{
    collect();
    xAxis.log = false;
    yAxis.numGraphs = zAxis.numGraphs = 0;
    double lo = NaN, hi = NaN;
    for (const Bar& b : std::as_const(m_bars)) {
        lo = std::isfinite(lo) ? std::min(lo, b.low) : b.low;
        hi = std::isfinite(hi) ? std::max(hi, b.high) : b.high;
    }
    if (!std::isfinite(lo) || !(hi > lo)) {
        lo = std::isfinite(lo) ? lo - 1 : -1;
        hi = std::isfinite(hi) ? hi + 1 : 1;
    }
    // From 0 both ways alike when the values go both ways; room for the
    // numbers at the bars' ends.
    if (mode == Value && lo < 0 && hi > 0) hi = -(lo = -std::max(-lo, hi));
    const double pad = 0.2 * (hi - lo);
    if (mode == Spread || lo < 0) lo -= pad;
    if (mode == Spread || hi > 0) hi += pad;
    xAxis.min = lo;
    xAxis.max = hi;
    yAxis.low = 0;
    yAxis.up = std::max<qsizetype>(1, m_bars.size());
    return RectDiagram::calcDiagram();
}

QList<Diagram::Part> TornadoDiagram::themeParts() const
{
    QList<Part> parts = RectDiagram::themeParts();
    parts.removeAll(Part::RightAxis);
    return parts;
}

void TornadoDiagram::createAxisLabels()
{
    // (No y label of the traces' names: the bars are named.)
    const QString x = xAxis.Label, y = yAxis.Label;
    if (y.isEmpty()) yAxis.Label = QStringLiteral(" ");
    if (x.isEmpty()) {
        // What the bars are: at which point, or over which sweep.
        QString sweep;
        double point = NaN;
        for (const Graph* g : std::as_const(Graphs))
            if (const DataX* xs = g->axis(0); xs != nullptr && xs->Points != nullptr && xs->count > 1) {
                sweep = bare(xs->Var);
                int i = 0;
                if (std::isfinite(at))
                    for (int j = 1; j < xs->count; ++j)
                        if (std::abs(xs->Points[j] - at) < std::abs(xs->Points[i] - at)) i = j;
                point = xs->Points[i];
                break;
            }
        if (mode == Spread) xAxis.Label = sweep.isEmpty() ? QObject::tr("spread") : QObject::tr("spread over %1").arg(sweep);
        else if (!sweep.isEmpty()) xAxis.Label = QObject::tr("at %1 = %2").arg(sweep, numberText(point));
    }
    RectDiagram::createAxisLabels();
    xAxis.Label = x;
    yAxis.Label = y;
}

QRectF TornadoDiagram::rowOf(int k) const
{
    const double h = double(y2) / std::max<qsizetype>(1, m_bars.size());
    return QRectF(0, y2 - (k + 1) * h, x2, h);
}

double TornadoDiagram::barsWidth(const QFontMetricsF& metrics) const
{
    double widest = 0;
    for (const Bar& b : m_bars) widest = std::max(widest, metrics.horizontalAdvance(b.name));
    return widest;
}

void TornadoDiagram::paintBehindGraphs(QPainter* painter)
{
    // (In its coordinates: origin at the lower left corner, y up.)
    painter->save();
    painter->setClipRect(QRectF(0, 0, x2, y2));
    const auto xOf = [&](double v) {
        const double y[2] = {0, 0};
        float px = 0, py = 0;
        calcCoordinate(&v, y, nullptr, &px, &py, &yAxis);
        return double(px);
    };
    const QFontMetricsF fm(painter->font());
    if (mode == Value && xAxis.low < 0 && xAxis.up > 0) {
        painter->setPen(QPen(QColor(90, 90, 90), 1));
        painter->drawLine(QPointF(xOf(0), 0), QPointF(xOf(0), y2));
    }
    for (int k = 0; k < m_bars.size(); ++k) {
        const Bar& b = m_bars.at(k);
        const QRectF row = rowOf(k);
        const double inset = std::min(0.18 * row.height(), 6.0);
        const QRectF bar(QPointF(xOf(b.low), row.top() + inset), QPointF(xOf(b.high), row.bottom() - inset));
        const QColor fill = mode == Spread || b.value >= 0 ? kUp : kDown;
        painter->setPen(QPen(fill.darker(130), 1));
        painter->setBrush(fill);
        painter->drawRect(bar.normalized());
        // A spread's value at the point: a line across it.
        if (mode == Spread) {
            painter->setPen(QPen(QColor(0, 0, 0), 2));
            painter->drawLine(QPointF(xOf(b.value), row.top() + inset / 2), QPointF(xOf(b.value), row.bottom() - inset / 2));
        }
        // Its number (four digits) at its outer end - inside it when there
        // is no room (a spread's: its width).
        const double shownValue = mode == Spread ? b.size() : b.value;
        const double scale = shownValue != 0 ? std::pow(10.0, 3 - std::floor(std::log10(std::fabs(shownValue)))) : 1.0;
        const QString text = numberText(&xAxis, std::round(shownValue * scale) / scale);
        const double w = fm.horizontalAdvance(text);
        const bool left = mode == Value && b.value < 0;
        const QRectF r = bar.normalized();
        double x = left ? r.left() - 4 - w : r.right() + 4;
        bool inside = false;
        if ((left && x < 0) || (!left && x + w > x2)) {
            x = left ? r.left() + 4 : r.right() - 4 - w;
            inside = true;
        }
        painter->save();
        painter->translate(x, row.center().y());
        painter->scale(1, -1);
        painter->setPen(inside ? QColor(255, 255, 255) : QColor(40, 40, 40));
        painter->drawText(QPointF(0, fm.ascent() / 2 - 1), text);
        painter->restore();
    }
    painter->restore();
}

void TornadoDiagram::paintInFront(QPainter* painter, const Colors& colors)
{
    RectDiagram::paintInFront(painter, colors);
    // (y down.) Each bar's name left of the frame; above it, what was left
    // out.
    painter->save();
    const QFontMetricsF fm(painter->font());
    painter->setPen(colors.of(Part::YAxis));
    for (int k = 0; k < m_bars.size(); ++k) {
        const QRectF row = rowOf(k);
        const QString& name = m_bars.at(k).name;
        painter->drawText(QPointF(-6 - fm.horizontalAdvance(name), -row.center().y() + fm.ascent() / 2 - 1), name);
    }
    QStringList left;
    if (m_nothing > 0) left << QObject::tr("%n of nothing", nullptr, m_nothing);
    if (m_smaller > 0) left << QObject::tr("%n smaller", nullptr, m_smaller);
    if (!m_noData.isEmpty()) left << QObject::tr("%n without data", nullptr, int(m_noData.size()));
    if (!left.isEmpty()) {
        const QString text = QObject::tr("not shown: %1").arg(left.join(QStringLiteral(", ")));
        painter->setPen(colors.of(Part::Text));
        painter->drawText(QPointF(x2 - fm.horizontalAdvance(text), -y2 - 4), text);
    }
    painter->restore();
}

QRectF TornadoDiagram::paintedRect(const QFontMetricsF& metrics) const
{
    QRectF r = RectDiagram::paintedRect(metrics);
    const double w = barsWidth(metrics);
    r |= QRectF(-w - 8, -y2 - metrics.height() - 6, w + 8 + x2, y2 + metrics.height() + 6).translated(cx, cy);
    return r;
}

QString TornadoDiagram::extraSaveFields() const
{
    return QStringLiteral(" %1 %2 %3").arg(mode).arg(fieldText(at)).arg(bars);
}

void TornadoDiagram::loadExtraFields(const QStringList& fields)
{
    bool ok = false;
    const int m = fields.value(0).toInt(&ok);
    mode = ok && (m == Value || m == Spread) ? m : Value;
    at = fieldValue(fields.value(1));
    const int n = fields.value(2).toInt(&ok);
    bars = ok && n >= 1 && n <= MaxBars ? n : 15;
}
