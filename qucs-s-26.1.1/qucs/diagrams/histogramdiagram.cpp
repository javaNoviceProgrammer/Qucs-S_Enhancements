/*
 * histogramdiagram.cpp - a histogram of the values of a variable (see
 * histogramdiagram.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "histogramdiagram.h"

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QPainter>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

QString tr(const char* text)
{
    return QCoreApplication::translate("HistogramDiagram", text);
}

// The quantile q of sorted values, interpolated between neighbours.
double quantile(const QVector<double>& sorted, double q)
{
    const double at = q * (sorted.size() - 1);
    const int i = int(std::floor(at));
    const int j = std::min(i + 1, int(sorted.size()) - 1);
    return sorted.at(i) + (at - i) * (sorted.at(j) - sorted.at(i));
}

QString limitText(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 12) : QStringLiteral("-");
}

double limitOf(const QString& text)
{
    bool ok = false;
    const double v = text.toDouble(&ok);
    return ok && std::isfinite(v) ? v : std::nan("");
}

} // namespace

HistogramDiagram::HistogramDiagram(int cx, int cy)
    : RectDiagram(cx, cy), lowerLimit(std::nan("")), upperLimit(std::nan(""))
{
    Name = "Histogram";
    xAxis.log = yAxis.log = zAxis.log = false;
    calcDiagram();
}

Diagram* HistogramDiagram::newOne()
{
    return new HistogramDiagram();
}

Element* HistogramDiagram::info(QString& Name, char*& BitmapFile, bool getNewOne)
{
    Name = QObject::tr("Histogram");
    BitmapFile = (char*) "histogram";
    if (getNewOne) return new HistogramDiagram();
    return nullptr;
}

QVector<double> HistogramDiagram::values(const Graph* graph)
{
    QVector<double> out;
    const DataX* x = graph->axis(0);
    if (graph->cPointsY == nullptr || x == nullptr || x->count <= 0 || graph->countY <= 0) return out;
    const double* p = graph->cPointsY;
    const qsizetype total = qsizetype(graph->countY) * x->count;
    out.reserve(total);
    for (qsizetype i = 0; i < total; ++i, p += 2) {
        const double v = std::fabs(p[1]) >= 1e-250 ? std::hypot(p[0], p[1]) : p[0];
        if (std::isfinite(v)) out << v;
    }
    return out;
}

int HistogramDiagram::automaticBins(const QVector<double>& values, double low, double high)
{
    const int n = int(values.size());
    if (n < 2 || !(high > low)) return 1;
    QVector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    const double iqr = quantile(sorted, 0.75) - quantile(sorted, 0.25);
    // As a double first: a tiny spread with a far value is billions of bins.
    const double count = iqr > 0.0 ? std::ceil((high - low) / (2.0 * iqr / std::cbrt(double(n))))
                                    : std::ceil(std::sqrt(double(n)));
    return int(std::clamp(count, 1.0, 200.0));
}

HistogramDiagram::Bars HistogramDiagram::bin(const QVector<double>& values, double low, double high, int count)
{
    Bars b;
    count = std::max(1, count);
    b.low = low;
    b.width = high > low ? (high - low) / count : 1.0;
    b.counts.fill(0.0, count);
    b.n = int(values.size());
    if (b.n == 0) return b;
    double sum = 0.0;
    b.min = b.max = values.first();
    for (double v : values) {
        sum += v;
        b.min = std::min(b.min, v);
        b.max = std::max(b.max, v);
        double k = std::floor((v - low) / b.width);   // a double: a far value is no int
        if (v == high) k = count - 1;   // the last bin holds its upper edge
        if (!(k >= 0.0 && k < count)) {
            ++b.outside;
            continue;
        }
        b.counts[int(k)] += 1.0;
    }
    b.mean = sum / b.n;
    double squares = 0.0;
    for (double v : values) squares += (v - b.mean) * (v - b.mean);
    b.sigma = b.n > 1 ? std::sqrt(squares / (b.n - 1)) : 0.0;
    b.within = b.n;
    return b;
}

double HistogramDiagram::scaleOf(const Bars& bars) const
{
    if (bars.n <= 0) return 0.0;
    if (height == Percent) return 100.0 / bars.n;
    if (height == Density) return bars.width > 0.0 ? 1.0 / (bars.n * bars.width) : 0.0;
    return 1.0;
}

void HistogramDiagram::getAxisLimits(Graph* pg)
{
    pg->yAxisNo = 0;   // one axis, the heights
    ++yAxis.numGraphs;
    for (double v : values(pg)) {
        xAxis.min = std::min(xAxis.min, v);
        xAxis.max = std::max(xAxis.max, v);
    }
}

int HistogramDiagram::calcDiagram()
{
    xAxis.log = yAxis.log = zAxis.log = false;
    zAxis.numGraphs = 0;
    m_bars.clear();

    QList<QVector<double>> each;
    QVector<double> pooled;
    for (Graph* g : Graphs) {
        each << values(g);
        pooled += each.last();
    }
    // The bins span the x axis' manual limits, or the values.
    double low = 0.0, high = 1.0;
    if (!xAxis.autoScale) {
        low = std::min(xAxis.limit_min, xAxis.limit_max);
        high = std::max(xAxis.limit_min, xAxis.limit_max);
    } else if (!pooled.isEmpty()) {
        const auto [lo, hi] = std::minmax_element(pooled.begin(), pooled.end());
        low = *lo;
        high = *hi;
    }
    if (!(high > low)) {   // a single value: a bin around it
        const double pad = low == 0.0 ? 0.5 : std::fabs(low) * 0.05;
        low -= pad;
        high += pad;
    }
    const int count = bins > 0 ? bins : automaticBins(pooled, low, high);

    double top = 0.0;
    for (const QVector<double>& v : each) {
        Bars b = bin(v, low, high, count);
        if (std::isfinite(lowerLimit) || std::isfinite(upperLimit)) {
            b.within = 0;
            for (double x : v)
                if ((!std::isfinite(lowerLimit) || x >= lowerLimit) && (!std::isfinite(upperLimit) || x <= upperLimit))
                    ++b.within;
        }
        const double f = scaleOf(b);
        for (double c : b.counts) top = std::max(top, c * f);
        if (normalFit && b.sigma > 0.0) top = std::max(top, f * b.n * b.width / (b.sigma * std::sqrt(2.0 * kPi)));
        m_bars << b;
    }
    if (xAxis.autoScale) {
        xAxis.min = low;
        xAxis.max = high;
    }
    // The heights start at 0: the automatic scale keeps a tenth of the
    // range free below and above the values, so a least value of a tenth
    // of the rest puts the axis' lower end on 0, and the top stays clear.
    yAxis.max = top > 0.0 ? top : 1.0;
    yAxis.min = yAxis.max / 11.0;
    return RectDiagram::calcDiagram();
}

QString HistogramDiagram::extraSaveFields() const
{
    const int flags = (normalFit ? 1 : 0) | (statistics ? 2 : 0);
    return QStringLiteral(" %1 %2 %3 %4 %5").arg(bins).arg(height).arg(flags).arg(limitText(lowerLimit), limitText(upperLimit));
}

void HistogramDiagram::loadExtraFields(const QStringList& fields)
{
    bool ok = false;
    const int b = fields.value(0).toInt(&ok);
    bins = ok && b >= 0 && b <= 10000 ? b : 0;
    const int h = fields.value(1).toInt(&ok);
    height = ok && h >= Counts && h <= Density ? h : Counts;
    const int flags = fields.value(2).toInt(&ok);
    if (ok) {
        normalFit = (flags & 1) != 0;
        statistics = (flags & 2) != 0;
    }
    lowerLimit = limitOf(fields.value(3));
    upperLimit = limitOf(fields.value(4));
}

QString HistogramDiagram::statisticText(double value) const
{
    // Four places of a mean or a deviation are plenty; a notation or places
    // chosen for the diagram hold here too.
    if (notation != qucs_s::numberformat::Notation::Automatic || notationDecimals >= 0) return numberText(value);
    return QString::number(value, 'g', 4);
}

QString HistogramDiagram::shortName(const Graph* graph) const
{
    // "ngspice/ngmontecarlo1.fc" is "fc", as the other diagrams' labels
    // leave out the simulator and the simulation.
    QString name = graph->Var;
    if (name.contains(QLatin1Char('/'))) {
        name = name.section(QLatin1Char('/'), 1);
        const int dot = int(name.indexOf(QLatin1Char('.')));
        const int paren = int(name.indexOf(QLatin1Char('(')));
        if (dot > 0 && (paren < 0 || dot < paren)) name = name.mid(dot + 1);
    }
    return name;
}

void HistogramDiagram::createAxisLabels()
{
    // The values' names under the x axis, what the heights are beside the
    // y axis - unless the diagram has labels of its own.
    const QString x = xAxis.Label, y = yAxis.Label;
    if (xAxis.Label.isEmpty()) {
        QStringList names;
        for (Graph* g : Graphs) names << shortName(g) + (g->cPointsY != nullptr ? QString() : INVALID_STR);
        xAxis.Label = names.join(QStringLiteral(", "));
    }
    if (yAxis.Label.isEmpty() && !Graphs.isEmpty())
        yAxis.Label = height == Percent ? tr("percent") : height == Density ? tr("probability density") : tr("count");
    Diagram::createAxisLabels();
    xAxis.Label = x;
    yAxis.Label = y;
}

void HistogramDiagram::paintBehindGraphs(QPainter* painter)
{
    if (m_bars.size() != Graphs.size() || !(xAxis.up != xAxis.low) || !(yAxis.up != yAxis.low)) return;
    auto X = [this](double v) { return (v - xAxis.low) / (xAxis.up - xAxis.low) * x2; };
    auto Y = [this](double v) { return (v - yAxis.low) / (yAxis.up - yAxis.low) * y2; };

    painter->save();
    painter->setClipRect(QRectF(0, 0, x2, y2));
    painter->setRenderHint(QPainter::Antialiasing, true);
    const int alpha = Graphs.size() > 1 ? 70 : 110;
    for (int i = 0; i < Graphs.size(); ++i) {
        const Graph* g = Graphs.at(i);
        const Bars& b = m_bars.at(i);
        const double f = scaleOf(b);
        QColor fill = g->Color;
        fill.setAlpha(alpha);
        QPen pen(g->Color, g->isSelected ? g->Thick + 2 : std::max(1, g->Thick));
        pen.setJoinStyle(Qt::MiterJoin);
        switch (g->Style) {   // the patterns of the graphs' lines
        case GRAPHSTYLE_DASH: pen.setDashPattern({10.0, 6.0}); break;
        case GRAPHSTYLE_DOT: pen.setDashPattern({2.0, 4.0}); break;
        case GRAPHSTYLE_LONGDASH: pen.setDashPattern({24.0, 8.0}); break;
        default: break;
        }
        for (int k = 0; k < b.counts.size(); ++k) {
            if (b.counts.at(k) <= 0.0) continue;
            const QRectF bar(QPointF(X(b.low + k * b.width), Y(0.0)),
                             QPointF(X(b.low + (k + 1) * b.width), Y(b.counts.at(k) * f)));
            painter->fillRect(bar.normalized(), fill);
            painter->setPen(pen);
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(bar.normalized());
        }
        if (normalFit && b.sigma > 0.0) {
            QPolygonF curve;
            const int steps = 200;
            for (int j = 0; j <= steps; ++j) {
                const double x = xAxis.low + (xAxis.up - xAxis.low) * j / steps;
                const double z = (x - b.mean) / b.sigma;
                const double pdf = std::exp(-0.5 * z * z) / (b.sigma * std::sqrt(2.0 * kPi));
                curve << QPointF(X(x), Y(f * b.n * b.width * pdf));
            }
            QPen fit(g->Color.darker(140), std::max(1.5, g->Thick * 1.0));
            fit.setDashPattern({6.0, 3.0});
            painter->setPen(fit);
            painter->drawPolyline(curve);
        }
    }
    QPen limit(QColor(200, 0, 0), 1.5);
    limit.setDashPattern({6.0, 4.0});
    painter->setPen(limit);
    for (double l : {lowerLimit, upperLimit})
        if (std::isfinite(l)) painter->drawLine(QPointF(X(l), 0.0), QPointF(X(l), y2));
    painter->restore();
}

void HistogramDiagram::paintInFront(QPainter* painter)
{
    if (!statistics || m_bars.isEmpty() || m_bars.size() != Graphs.size()) return;
    const bool limits = std::isfinite(lowerLimit) || std::isfinite(upperLimit);
    QStringList rows;
    for (int i = 0; i < Graphs.size(); ++i) {
        const Bars& b = m_bars.at(i);
        QString row = QStringLiteral("%1:  N %2   mean %3   %4 %5")
                          .arg(shortName(Graphs.at(i)))
                          .arg(b.n)
                          .arg(statisticText(b.mean), QString(QChar(0x03C3)), statisticText(b.sigma));
        if (limits && b.n > 0) row += QStringLiteral("   ") + tr("within %1%").arg(100.0 * b.within / b.n, 0, 'f', 1);
        if (b.outside > 0) row += QStringLiteral("   ") + tr("%1 outside the bins").arg(b.outside);
        rows << row;
    }

    const QFontMetricsF fm(painter->font());
    const qreal pad = 4.0, swatch = 10.0, gap = 5.0, margin = 6.0;
    qreal textWidth = 0.0;
    for (const QString& r : rows) textWidth = std::max(textWidth, fm.horizontalAdvance(r));
    const qreal width = pad + swatch + gap + textWidth + pad;
    const qreal height = pad + rows.size() * fm.height() + pad;
    // In the top corner the bars reach the least into (not the legend's).
    const qreal y = -y2 + margin;
    auto intrusion = [&](qreal left) {
        if ((left < x2 / 2.0 && legendPos == LegendTopLeft) || (left >= x2 / 2.0 && legendPos == LegendTopRight))
            return 1e300;
        const double from = xAxis.low + (left / x2) * (xAxis.up - xAxis.low);
        const double to = xAxis.low + ((left + width) / x2) * (xAxis.up - xAxis.low);
        double tallest = 0.0;
        for (const Bars& b : m_bars) {
            const double f = scaleOf(b);
            for (int k = 0; k < b.counts.size(); ++k) {
                const double lo = b.low + k * b.width, hi = lo + b.width;
                if (hi > from && lo < to) tallest = std::max(tallest, b.counts.at(k) * f);
            }
        }
        return tallest;
    };
    const qreal right = x2 - margin - width;
    const qreal x = intrusion(margin) < intrusion(right) ? margin : right;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QPen(Qt::darkGray, 1));
    painter->setBrush(QColor(255, 255, 255, 230));
    painter->drawRect(QRectF(x, y, width, height));
    qreal top = y + pad;
    for (int i = 0; i < rows.size(); ++i) {
        const qreal mid = top + fm.height() / 2.0;
        QColor fill = Graphs.at(i)->Color;
        painter->setPen(QPen(fill, 1));
        fill.setAlpha(110);
        painter->setBrush(fill);
        painter->drawRect(QRectF(x + pad, mid - swatch / 2, swatch, swatch));
        painter->setPen(Qt::black);
        painter->drawText(QPointF(x + pad + swatch + gap, top + fm.ascent()), rows.at(i));
        top += fm.height();
    }
    painter->restore();
}
