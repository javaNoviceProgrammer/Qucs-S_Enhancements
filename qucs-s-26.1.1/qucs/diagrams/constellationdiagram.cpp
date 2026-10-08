/*
 * constellationdiagram.cpp - the constellation (IQ) diagram (see
 * constellationdiagram.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "constellationdiagram.h"

#include "ink.h"

#include <QFontMetricsF>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
constexpr double Pi = 3.14159265358979323846;
constexpr int kMostPoints = 20000;

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

// A trace's first curve: its x and (a complex value's magnitude) its y.
void curveOf(const Graph* g, QVector<double>* x, QVector<double>* y)
{
    const DataX* xs = g->axis(0);
    if (g->cPointsY == nullptr || xs == nullptr || xs->Points == nullptr) return;
    for (int i = 0; i < xs->count; ++i) {
        const double re = g->cPointsY[2 * i], im = g->cPointsY[2 * i + 1];
        *x << xs->Points[i];
        *y << (std::fabs(im) > 1e-250 ? std::hypot(re, im) : re);
    }
}

double at(const QVector<double>& x, const QVector<double>& y, double t)
{
    const int i = int(std::upper_bound(x.cbegin(), x.cend(), t) - x.cbegin());
    if (i <= 0) return y.first();
    if (i >= x.size()) return y.last();
    const double f = x.at(i) != x.at(i - 1) ? (t - x.at(i - 1)) / (x.at(i) - x.at(i - 1)) : 0.0;
    return y.at(i - 1) + f * (y.at(i) - y.at(i - 1));
}

} // namespace

ConstellationDiagram::ConstellationDiagram(int cx, int cy) : RectDiagram(cx, cy), period(NaN), offset(NaN), from(NaN)
{
    x2 = 260;
    y2 = 260;
    x3 = x2 + 7;
    Name = "Constellation";
    calcDiagram();
}

Diagram* ConstellationDiagram::newOne()
{
    return new ConstellationDiagram();
}

Element* ConstellationDiagram::info(QString& Name, char*& BitmapFile, bool getNewOne)
{
    Name = QObject::tr("Constellation");
    BitmapFile = (char*)"constellation";
    if (getNewOne) return new ConstellationDiagram();
    return nullptr;
}

QStringList ConstellationDiagram::modulationNames()
{
    return {QStringLiteral("none"), QStringLiteral("bpsk"), QStringLiteral("qpsk"), QStringLiteral("8psk"), QStringLiteral("16qam"),
            QStringLiteral("64qam")};
}

QList<Diagram::Part> ConstellationDiagram::themeParts() const
{
    QList<Part> parts = RectDiagram::themeParts();
    parts.removeAll(Part::RightAxis);
    return parts;
}

QList<QPointF> ConstellationDiagram::idealPoints(int modulation)
{
    QList<QPointF> p;
    switch (modulation) {
    case Bpsk: p = {QPointF(-1, 0), QPointF(1, 0)}; break;
    case Qpsk:
        for (int k = 0; k < 4; ++k) p << QPointF(std::cos(Pi / 4 + k * Pi / 2), std::sin(Pi / 4 + k * Pi / 2));
        break;
    case Psk8:
        for (int k = 0; k < 8; ++k) p << QPointF(std::cos(k * Pi / 4), std::sin(k * Pi / 4));
        break;
    case Qam16:
    case Qam64: {
        // A square grid of odd levels, scaled to an rms of 1.
        const int side = modulation == Qam16 ? 4 : 8;
        double squares = 0;
        for (int i = 0; i < side; ++i)
            for (int j = 0; j < side; ++j) {
                const QPointF q(2 * i - side + 1, 2 * j - side + 1);
                p << q;
                squares += q.x() * q.x() + q.y() * q.y();
            }
        const double rms = std::sqrt(squares / p.size());
        for (QPointF& q : p) q /= rms;
        break;
    }
    default: break;
    }
    return p;
}

ConstellationDiagram::Pair ConstellationDiagram::pairOf(const Graph* gi, const Graph* gq) const
{
    Pair pair;
    pair.i = gi->Var.section(QLatin1Char('/'), -1);
    pair.q = gq != nullptr ? gq->Var.section(QLatin1Char('/'), -1) : QString();
    if (gq == nullptr) {
        pair.error = QObject::tr("an I with no Q: the traces go in pairs, I then Q");
        return pair;
    }
    QVector<double> ti, vi, tq, vq;
    curveOf(gi, &ti, &vi);
    curveOf(gq, &tq, &vq);
    return sampled(pair.i, pair.q, ti, vi, tq, vq, period, offset, from, modulation);
}

ConstellationDiagram::Pair ConstellationDiagram::sampled(const QString& i, const QString& q, const QVector<double>& ti,
                                                         const QVector<double>& vi, const QVector<double>& tq,
                                                         const QVector<double>& vq, double period, double offset,
                                                         double from, int modulation)
{
    Pair pair;
    pair.i = i;
    pair.q = q;
    if (ti.size() < 2 || tq.size() < 2) {
        pair.error = QObject::tr("no data: simulate, or check the variables' names");
        return pair;
    }
    const double start = std::max(ti.first(), tq.first()), end = std::min(ti.last(), tq.last());
    const double first = std::isfinite(from) ? std::max(start, from) : start;
    // What is at fault when nothing is sampled: said by its name (it was
    // "no sample from 0 on" whatever it was - bug hunt of 2026-10-08, N21).
    if (std::isfinite(from) && from > end) {
        pair.error = QObject::tr("'from' (%1) is past the run's end (%2)").arg(from).arg(end);
        return pair;
    }
    if (std::isfinite(period) && period > 0 && std::isfinite(offset) && offset > end) {
        pair.error = QObject::tr("the offset (%1) is past the run's end (%2)").arg(offset).arg(end);
        return pair;
    }
    if (std::isfinite(period) && period > end - first) {
        pair.error = QObject::tr("a symbol period of %1 is longer than the run from %2 to %3").arg(period).arg(first).arg(end);
        return pair;
    }
    double reached = end;
    if (std::isfinite(period) && period > 0) {
        // Once a symbol: from the offset (else half a period in).
        double t = std::isfinite(offset) ? offset : start + period / 2;
        if (t < first) t += std::ceil((first - t) / period) * period;
        for (; t <= end && pair.points.size() < kMostPoints; t += period) {
            pair.points << QPointF(at(ti, vi, t), at(tq, vq, t));
            reached = t;
        }
        if (pair.points.size() >= kMostPoints && reached + period <= end)
            pair.note = QObject::tr("the first %1 symbols only, to %2 of the run's %3 (at most %1 are taken: a longer symbol period, "
                                    "or a later 'from', takes others)").arg(kMostPoints).arg(reached).arg(end);
    } else {
        // Every sample of I, Q there.
        for (int k = 0; k < ti.size() && pair.points.size() < kMostPoints; ++k)
            if (ti.at(k) >= first && ti.at(k) <= end) {
                pair.points << QPointF(vi.at(k), at(tq, vq, ti.at(k)));
                reached = ti.at(k);
            }
        if (pair.points.size() >= kMostPoints && reached < end)
            pair.note = QObject::tr("the first %1 samples only, to %2 of the run's %3 (at most %1 are taken: a symbol period, or a "
                                    "later 'from', takes others)").arg(kMostPoints).arg(reached).arg(end);
    }
    if (pair.points.isEmpty()) {
        pair.error = QObject::tr("no sample from %1 on").arg(first);
        return pair;
    }
    double si = 0, sq = 0, power = 0;
    for (const QPointF& p : std::as_const(pair.points)) {
        si += p.x();
        sq += p.y();
        power += p.x() * p.x() + p.y() * p.y();
    }
    pair.centre = QPointF(si / pair.points.size(), sq / pair.points.size());
    // The ideal points at the samples' rms; each sample's error to the
    // nearest.
    const double rms = std::sqrt(power / pair.points.size());
    for (const QPointF& p : idealPoints(modulation)) pair.ideal << p * rms;
    if (!pair.ideal.isEmpty() && rms > 0) {
        double errors = 0, peak = 0;
        for (const QPointF& p : std::as_const(pair.points)) {
            double nearest = 1e300;
            for (const QPointF& q : std::as_const(pair.ideal)) nearest = std::min(nearest, std::hypot(p.x() - q.x(), p.y() - q.y()));
            errors += nearest * nearest;
            peak = std::max(peak, nearest);
        }
        pair.evmRms = 100 * std::sqrt(errors / pair.points.size()) / rms;
        pair.evmPeak = 100 * peak / rms;
    }
    return pair;
}

void ConstellationDiagram::getAxisLimits(Graph* g)
{
    g->yAxisNo = 0;
    ++yAxis.numGraphs;
}

int ConstellationDiagram::calcDiagram()
{
    m_pairs.clear();
    for (int k = 0; k < Graphs.size(); k += 2) m_pairs << pairOf(Graphs.at(k), k + 1 < Graphs.size() ? Graphs.at(k + 1) : nullptr);
    // I along, Q up, on one scale about 0: to the farthest point (an ideal
    // one too), a tenth more.
    double reach = 0;
    for (const Pair& p : std::as_const(m_pairs)) {
        for (const QPointF& q : p.points) reach = std::max({reach, std::abs(q.x()), std::abs(q.y())});
        for (const QPointF& q : p.ideal) reach = std::max({reach, std::abs(q.x()), std::abs(q.y())});
    }
    if (!(reach > 0)) reach = 1;
    xAxis.log = yAxis.log = false;
    xAxis.min = yAxis.min = -1.1 * reach;
    xAxis.max = yAxis.max = 1.1 * reach;
    // Both automatic: laid out alike - the same round step and ends - for
    // one scale (each rounded by itself, they need not be).
    if (!xAxis.autoScale || !yAxis.autoScale) return RectDiagram::calcDiagram();
    const double raw = 2.2 * reach / 4;
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    double step = 10 * magnitude;
    for (double m : {1.0, 2.0, 2.5, 5.0})
        if (m * magnitude >= raw) {
            step = m * magnitude;
            break;
        }
    const double end = std::ceil(1.1 * reach / step) * step;
    for (Axis* a : {&xAxis, &yAxis}) {
        a->autoScale = false;
        a->limit_min = -end;
        a->limit_max = end;
        a->step = step;
    }
    const int valid = RectDiagram::calcDiagram();
    xAxis.autoScale = yAxis.autoScale = true;
    return valid;
}

void ConstellationDiagram::createAxisLabels()
{
    const QString x = xAxis.Label, y = yAxis.Label;
    if (!m_pairs.isEmpty()) {
        if (x.isEmpty()) xAxis.Label = QStringLiteral("I: %1").arg(m_pairs.first().i);
        if (y.isEmpty()) yAxis.Label = QStringLiteral("Q: %1").arg(m_pairs.first().q);
    }
    RectDiagram::createAxisLabels();
    xAxis.Label = x;
    yAxis.Label = y;
}

void ConstellationDiagram::paintBehindGraphs(QPainter* painter)
{
    // (In its coordinates: origin at the lower left corner, y up.)
    painter->save();
    painter->setClipRect(QRectF(0, 0, x2, y2));
    painter->setRenderHint(QPainter::Antialiasing, true);
    const auto place = [&](const QPointF& p) {
        const double x = p.x(), y[2] = {p.y(), 0};
        float px = 0, py = 0;
        calcCoordinate(&x, y, nullptr, &px, &py, &yAxis);
        return QPointF(px, py);
    };
    // The axes through 0.
    painter->setPen(QPen(QColor(150, 150, 150), 1));
    painter->drawLine(place(QPointF(xAxis.low, 0)), place(QPointF(xAxis.up, 0)));
    painter->drawLine(place(QPointF(0, yAxis.low)), place(QPointF(0, yAxis.up)));
    for (int k = 0; k < m_pairs.size() && 2 * k < Graphs.size(); ++k) {
        const Pair& p = m_pairs.at(k);
        if (!p.ok()) continue;
        QColor ink = qucs_s::ink::on(Graphs.at(2 * k)->Color);
        // The samples: dots, faint where many fall on one place.
        QColor dot = ink;
        dot.setAlpha(p.points.size() > 500 ? 70 : 170);
        painter->setPen(Qt::NoPen);
        painter->setBrush(dot);
        for (const QPointF& q : p.points) painter->drawEllipse(place(q), 2.0, 2.0);
        // The ideal points: crosses.
        painter->setPen(QPen(QColor(0, 0, 0), 2));
        for (const QPointF& q : p.ideal) {
            const QPointF c = place(q);
            painter->drawLine(c + QPointF(-6, -6), c + QPointF(6, 6));
            painter->drawLine(c + QPointF(-6, 6), c + QPointF(6, -6));
        }
    }
    painter->restore();
}

void ConstellationDiagram::paintInFront(QPainter* painter, const Colors& colors)
{
    RectDiagram::paintInFront(painter, colors);
    QStringList lines;
    for (const Pair& p : std::as_const(m_pairs)) {
        if (!p.ok()) {
            lines << QStringLiteral("%1: %2").arg(p.i, p.error);
            continue;
        }
        QString line = QObject::tr("%1 symbols").arg(p.points.size());
        if (!p.ideal.isEmpty())
            line += QObject::tr(", EVM %1 % rms, %2 % peak").arg(QString::number(p.evmRms, 'f', 2), QString::number(p.evmPeak, 'f', 2));
        lines << line;
    }
    if (lines.isEmpty()) return;
    // (y down.) In the upper left corner.
    painter->save();
    const QFontMetricsF fm(painter->font());
    double width = 0;
    for (const QString& l : lines) width = std::max(width, fm.horizontalAdvance(l));
    const QRectF box(4, -y2 + 4, width + 8, lines.size() * fm.height() + 6);
    painter->setPen(QPen(colors.of(Part::LegendBorder), 1));
    painter->setBrush(colors.of(Part::LegendBackground));
    painter->drawRect(box);
    painter->setPen(colors.of(Part::LegendText));
    double y = box.top() + 3 + fm.ascent();
    for (const QString& l : lines) {
        painter->drawText(QPointF(box.left() + 4, y), l);
        y += fm.height();
    }
    painter->restore();
}

QString ConstellationDiagram::extraSaveFields() const
{
    return QStringLiteral(" %1 %2 %3 %4").arg(fieldText(period), fieldText(offset), fieldText(from)).arg(modulation);
}

void ConstellationDiagram::loadExtraFields(const QStringList& fields)
{
    period = fieldValue(fields.value(0));
    if (!(period > 0)) period = NaN;
    offset = fieldValue(fields.value(1));
    from = fieldValue(fields.value(2));
    bool ok = false;
    const int m = fields.value(3).toInt(&ok);
    modulation = ok && m >= None && m <= Qam64 ? m : None;
}
