/*
 * boxplotdiagram.cpp - the box plot: each trace's spread of values (see
 * boxplotdiagram.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "boxplotdiagram.h"

#include "ink.h"

#include <QFontMetricsF>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <numeric>

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

// The q-quantile of \a sorted, between its samples.
double quantile(const QVector<double>& sorted, double q)
{
    const double h = (sorted.size() - 1) * q;
    const int i = int(std::floor(h));
    if (i + 1 >= sorted.size()) return sorted.last();
    return sorted.at(i) + (h - i) * (sorted.at(i + 1) - sorted.at(i));
}

} // namespace

BoxPlotDiagram::BoxPlotDiagram(int cx, int cy) : RectDiagram(cx, cy), at(NaN)
{
    x2 = 300;
    y2 = 220;
    x3 = x2 + 7;
    Name = "BoxPlot";
    calcDiagram();
}

Diagram* BoxPlotDiagram::newOne()
{
    return new BoxPlotDiagram();
}

Element* BoxPlotDiagram::info(QString& Name, char*& BitmapFile, bool getNewOne)
{
    Name = QObject::tr("Box Plot");
    BitmapFile = (char*)"boxplot";
    if (getNewOne) return new BoxPlotDiagram();
    return nullptr;
}

QList<Diagram::Part> BoxPlotDiagram::themeParts() const
{
    QList<Part> parts = RectDiagram::themeParts();
    parts.removeAll(Part::RightAxis);
    return parts;
}

BoxPlotDiagram::Box BoxPlotDiagram::boxOf(QVector<double> values, bool range)
{
    Box b;
    values.erase(std::remove_if(values.begin(), values.end(), [](double v) { return !std::isfinite(v); }), values.end());
    if (values.isEmpty()) {
        b.error = QObject::tr("no values");
        return b;
    }
    std::sort(values.begin(), values.end());
    b.n = int(values.size());
    b.min = values.first();
    b.max = values.last();
    b.q1 = quantile(values, 0.25);
    b.median = quantile(values, 0.5);
    b.q3 = quantile(values, 0.75);
    b.mean = std::accumulate(values.cbegin(), values.cend(), 0.0) / b.n;
    double squares = 0;
    for (double v : values) squares += (v - b.mean) * (v - b.mean);
    b.sd = b.n > 1 ? std::sqrt(squares / (b.n - 1)) : 0.0;
    if (range) {
        b.low = b.min;
        b.high = b.max;
        return b;
    }
    // Tukey's: the farthest values within 1.5 IQR of the box; the rest out.
    const double iqr = b.q3 - b.q1, lowFence = b.q1 - 1.5 * iqr, highFence = b.q3 + 1.5 * iqr;
    b.low = b.q1;
    b.high = b.q3;
    for (double v : std::as_const(values)) {
        if (v < lowFence || v > highFence) b.outliers << v;
        else {
            b.low = std::min(b.low, v);
            b.high = std::max(b.high, v);
        }
    }
    return b;
}

QVector<double> BoxPlotDiagram::valuesOf(const Graph* g) const
{
    QVector<double> out;
    const DataX* x = g->axis(0);
    if (g->cPointsY == nullptr || x == nullptr || x->Points == nullptr || x->count <= 0 || g->countY <= 0) return out;
    const auto valueAt = [&](qint64 i) {
        const double re = g->cPointsY[2 * i], im = g->cPointsY[2 * i + 1];
        return std::fabs(im) >= 1e-250 ? std::hypot(re, im) : re;
    };
    if (g->countY == 1) {
        // One curve: all of its values (one per run).
        for (int i = 0; i < x->count; ++i) out << valueAt(i);
        return out;
    }
    // Several: each curve's at x (between its samples), else its last.
    for (int c = 0; c < g->countY; ++c) {
        const qint64 base = qint64(c) * x->count;
        if (!std::isfinite(at) || x->count < 2) {
            out << valueAt(base + x->count - 1);
            continue;
        }
        int i = 0;
        while (i + 2 < x->count && x->Points[i + 1] < at) ++i;
        const double x0 = x->Points[i], x1 = x->Points[i + 1];
        const double f = x1 != x0 ? std::clamp((at - x0) / (x1 - x0), 0.0, 1.0) : 0.0;
        out << valueAt(base + i) + f * (valueAt(base + i + 1) - valueAt(base + i));
    }
    return out;
}

void BoxPlotDiagram::getAxisLimits(Graph* g)
{
    g->yAxisNo = 0;
    ++yAxis.numGraphs;
    // Up y: every value a box is of (its outliers too).
    for (double v : valuesOf(g))
        if (std::isfinite(v)) {
            yAxis.min = std::min(yAxis.min, v);
            yAxis.max = std::max(yAxis.max, v);
        }
}

int BoxPlotDiagram::calcDiagram()
{
    m_boxes.clear();
    for (const Graph* g : Graphs) {
        Box b = boxOf(valuesOf(g), range);
        b.name = g->Var.section(QLatin1Char('/'), -1);
        m_boxes << b;
    }
    // Along: a column a box (no numbers on it: the boxes are named).
    xAxis.log = false;
    xAxis.min = 0;
    xAxis.max = std::max<qsizetype>(1, m_boxes.size());
    xAxis.autoScale = true;
    xAxis.GridOn = false;
    const int valid = RectDiagram::calcDiagram();
    // The x axis' numbers and ticks: none (a column is a box, not a value).
    for (int i = int(Texts.size()) - 1; i >= 0; --i)
        if (Texts.at(i)->part == static_cast<unsigned char>(Part::XAxis)) delete Texts.takeAt(i);
    for (int i = int(Lines.size()) - 1; i >= 0; --i)
        if (Lines.at(i)->part == static_cast<unsigned char>(Part::XAxis)) delete Lines.takeAt(i);
    return valid;
}

double BoxPlotDiagram::columnOf(int k) const
{
    const int n = std::max<qsizetype>(1, m_boxes.size());
    return (k + 0.5) * double(x2) / n;
}

void BoxPlotDiagram::createAxisLabels()
{
    // (No x label of the traces' names: the boxes are named under them.)
    const QString x = xAxis.Label;
    if (x.isEmpty()) xAxis.Label = QStringLiteral(" ");
    RectDiagram::createAxisLabels();
    xAxis.Label = x;
}

void BoxPlotDiagram::paintBehindGraphs(QPainter* painter)
{
    // (In its coordinates: origin at the lower left corner, y up.)
    painter->save();
    painter->setClipRect(QRectF(0, 0, x2, y2));
    painter->setRenderHint(QPainter::Antialiasing, true);
    const auto yOf = [&](double v) {
        const double x = 0, yd[2] = {v, 0};
        float px = 0, py = 0;
        calcCoordinate(&x, yd, nullptr, &px, &py, &yAxis);
        return double(py);
    };
    const int n = std::max<qsizetype>(1, m_boxes.size());
    const double width = std::min(60.0, 0.5 * x2 / n);
    for (int k = 0; k < m_boxes.size() && k < Graphs.size(); ++k) {
        const Box& b = m_boxes.at(k);
        if (!b.ok()) continue;
        const double cx = columnOf(k);
        const QColor ink = qucs_s::ink::on(Graphs.at(k)->Color);
        QColor fill = ink;
        fill.setAlpha(60);
        // The whiskers, their caps, then the box over them.
        painter->setPen(QPen(ink, 1.5));
        painter->drawLine(QPointF(cx, yOf(b.low)), QPointF(cx, yOf(b.q1)));
        painter->drawLine(QPointF(cx, yOf(b.q3)), QPointF(cx, yOf(b.high)));
        painter->drawLine(QPointF(cx - width / 4, yOf(b.low)), QPointF(cx + width / 4, yOf(b.low)));
        painter->drawLine(QPointF(cx - width / 4, yOf(b.high)), QPointF(cx + width / 4, yOf(b.high)));
        painter->setBrush(fill);
        painter->drawRect(QRectF(QPointF(cx - width / 2, yOf(b.q1)), QPointF(cx + width / 2, yOf(b.q3))).normalized());
        painter->setPen(QPen(ink, 3));
        painter->drawLine(QPointF(cx - width / 2, yOf(b.median)), QPointF(cx + width / 2, yOf(b.median)));
        // The mean, a diamond; the outliers, circles.
        painter->setPen(QPen(ink, 1.5));
        painter->setBrush(Qt::NoBrush);
        const QPointF m(cx, yOf(b.mean));
        painter->drawPolygon(QPolygonF({m + QPointF(-4, 0), m + QPointF(0, 4), m + QPointF(4, 0), m + QPointF(0, -4)}));
        for (double v : b.outliers) painter->drawEllipse(QPointF(cx, yOf(v)), 3.0, 3.0);
    }
    painter->restore();
}

void BoxPlotDiagram::paintInFront(QPainter* painter, const Colors& colors)
{
    RectDiagram::paintInFront(painter, colors);
    // (y down.) Each box's name under its column, and how many values.
    painter->save();
    const QFontMetricsF fm(painter->font());
    painter->setPen(colors.of(Part::XAxis));
    for (int k = 0; k < m_boxes.size(); ++k) {
        const Box& b = m_boxes.at(k);
        const QString text = b.ok() ? QStringLiteral("%1 (%2)").arg(b.name).arg(b.n) : QStringLiteral("%1: %2").arg(b.name, b.error);
        painter->drawText(QPointF(columnOf(k) - fm.horizontalAdvance(text) / 2, fm.ascent() + 4), text);
    }
    painter->restore();
}

QString BoxPlotDiagram::extraSaveFields() const
{
    return QStringLiteral(" %1 %2").arg(fieldText(at)).arg(range ? 1 : 0);
}

void BoxPlotDiagram::loadExtraFields(const QStringList& fields)
{
    at = fieldValue(fields.value(0));
    range = fields.value(1) == QLatin1String("1");
}
