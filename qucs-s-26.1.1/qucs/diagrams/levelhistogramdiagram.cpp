/*
 * levelhistogramdiagram.cpp - the level histogram: a serial data signal's
 * values at the sampling instant, counted, beside its eye
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "levelhistogramdiagram.h"

#include "dataset.h"
#include "histogramdiagram.h"
#include "ink.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace eye = qucs_s::eye;

namespace {

constexpr double kPi = 3.14159265358979323846;

QString fieldText(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 12) : QStringLiteral("-");
}

double fieldValue(const QString& text)
{
    bool ok = false;
    const double v = text.toDouble(&ok);
    return ok && std::isfinite(v) ? v : eye::NaN;
}

QString rateText(double rate)
{
    return QString::number(rate, 'g', 3);
}

QString nameOf(const Graph* g)
{
    return g->Var.section(QLatin1Char('/'), -1);
}

// The quantile q of sorted values, between neighbours.
double quantile(const QVector<double>& sorted, double q)
{
    const double at = q * (sorted.size() - 1);
    const int i = int(std::floor(at));
    const int j = std::min(i + 1, int(sorted.size()) - 1);
    return sorted.at(i) + (at - i) * (sorted.at(j) - sorted.at(i));
}

} // namespace

LevelHistogramDiagram::LevelHistogramDiagram(int cx, int cy)
    : RectDiagram(cx, cy), ui(eye::NaN), start(eye::NaN), threshold(eye::NaN)
{
    x2 = 300;
    y2 = 260;
    x3 = x2 + 7;
    Name = "LevelHistogram";
    xAxis.log = yAxis.log = zAxis.log = false;
    calcDiagram();
}

Diagram* LevelHistogramDiagram::newOne()
{
    return new LevelHistogramDiagram();
}

Element* LevelHistogramDiagram::info(QString& Name, char*& BitmapFile, bool getNewOne)
{
    Name = QObject::tr("Level Histogram");
    BitmapFile = (char*)"levelhistogram";
    if (getNewOne) return new LevelHistogramDiagram();
    return nullptr;
}

QList<Diagram::Part> LevelHistogramDiagram::themeParts() const
{
    QList<Part> parts = RectDiagram::themeParts();
    parts.removeAll(Part::RightAxis);
    return parts;
}

int LevelHistogramDiagram::levelBins(const QList<eye::Level>& levels, double low, double high)
{
    if (!(high > low)) return 1;
    double width = eye::NaN;
    int n = 0;
    for (const eye::Level& l : levels) {
        n += l.count();
        if (l.count() < 2) continue;
        const double iqr = quantile(l.values, 0.75) - quantile(l.values, 0.25);
        if (!(iqr > 0.0)) continue;
        const double w = 2.0 * iqr / std::cbrt(double(l.count()));
        width = std::isfinite(width) ? std::min(width, w) : w;
    }
    // (As a double first: a tiny spread with a far level is billions of bins.)
    const double count = std::isfinite(width) ? std::ceil((high - low) / width) : std::ceil(std::sqrt(double(std::max(n, 1))));
    return int(std::clamp(count, 1.0, 400.0));
}

void LevelHistogramDiagram::getAxisLimits(Graph* g)
{
    g->yAxisNo = 0;   // one axis, the signal's
    ++yAxis.numGraphs;
}

int LevelHistogramDiagram::calcDiagram()
{
    xAxis.log = yAxis.log = zAxis.log = false;
    zAxis.numGraphs = 0;
    m_histograms.clear();

    eye::Options o;
    o.start = start;
    o.threshold = threshold;
    const EyeDiagram::Folding folding = EyeDiagram::foldingOf(this, ui, levels, o);
    QList<QVector<double>> each;
    QList<eye::Level> pooled;
    double low = eye::NaN, high = eye::NaN;
    for (int i = 0; i < folding.results.size(); ++i) {
        const eye::Result& r = folding.results.at(i);
        Histogram h;
        QVector<double> sampled;
        if (!r.ok()) {
            h.error = r.error;
        } else {
            sampled = eye::sampledAt(folding.curves.value(i), r, phase);
            h.levels = eye::levelsOf(r, sampled);
            for (int k = 0; k < r.eyes.size(); ++k) {
                h.eyes << eye::voltageBathtubOf(r, k, sampled, phase);
                h.thresholds << r.eyes.at(k).threshold;
            }
            h.symbols = int(sampled.size());
            pooled += h.levels;
            for (double v : std::as_const(sampled)) {
                low = std::isfinite(low) ? std::min(low, v) : v;
                high = std::isfinite(high) ? std::max(high, v) : v;
            }
        }
        each << sampled;
        m_histograms << h;
    }
    // The bins span the signal's axis' manual limits, or the values.
    if (!yAxis.autoScale) {
        low = std::min(yAxis.limit_min, yAxis.limit_max);
        high = std::max(yAxis.limit_min, yAxis.limit_max);
    }
    if (!std::isfinite(low)) {
        low = 0.0;
        high = 1.0;
    }
    if (!(high > low)) {   // a single value: a bin around it
        const double pad = low == 0.0 ? 0.5 : std::fabs(low) * 0.05;
        low -= pad;
        high += pad;
    }
    const int count = bins > 0 ? bins : levelBins(pooled, low, high);
    double top = 0.0;
    for (int i = 0; i < m_histograms.size(); ++i) {
        Histogram& h = m_histograms[i];
        const HistogramDiagram::Bars b = HistogramDiagram::bin(each.at(i), low, high, count);
        h.low = b.low;
        h.width = b.width;
        h.counts = b.counts;
        for (double c : std::as_const(h.counts)) top = std::max(top, c);
        if (gaussians)
            for (const eye::Level& l : std::as_const(h.levels))
                if (l.sigma > 0.0) top = std::max(top, l.count() * h.width / (l.sigma * std::sqrt(2.0 * kPi)));
    }
    if (yAxis.autoScale) {
        yAxis.min = low;
        yAxis.max = high;
    }
    // The counts from 0, a little room past the longest bar (the x axis is
    // scaled to its values, as they are).
    xAxis.min = 0.0;
    xAxis.max = top > 0.0 ? 1.05 * top : 1.0;
    return RectDiagram::calcDiagram();
}

void LevelHistogramDiagram::createAxisLabels()
{
    const QString x = xAxis.Label, y = yAxis.Label;
    if (x.isEmpty()) xAxis.Label = QObject::tr("symbols");
    if (y.isEmpty()) {
        const QString at = phase == 0.0 ? QObject::tr("at the eye's centre")
                                        : QObject::tr("%1 UI from the eye's centre").arg((phase > 0 ? QStringLiteral("+") : QString())
                                                                                         + QString::number(phase, 'g', 3));
        yAxis.Label = Graphs.size() == 1 ? QStringLiteral("%1 %2").arg(nameOf(Graphs.first()), at) : QObject::tr("the signal %1").arg(at);
    }
    RectDiagram::createAxisLabels();
    xAxis.Label = x;
    yAxis.Label = y;
}

void LevelHistogramDiagram::paintBehindGraphs(QPainter* painter)
{
    if (m_histograms.size() != Graphs.size() || !(xAxis.up != xAxis.low) || !(yAxis.up != yAxis.low)) return;
    // (In its coordinates: origin at the lower left corner, y up; the counts
    // across, the signal up.)
    auto X = [this](double count) { return (count - xAxis.low) / (xAxis.up - xAxis.low) * x2; };
    auto Y = [this](double v) { return (v - yAxis.low) / (yAxis.up - yAxis.low) * y2; };
    painter->save();
    painter->setClipRect(QRectF(0, 0, x2, y2));
    painter->setRenderHint(QPainter::Antialiasing, true);
    const int alpha = Graphs.size() > 1 ? 70 : 110;
    for (int i = 0; i < Graphs.size(); ++i) {
        const Graph* g = Graphs.at(i);
        const Histogram& h = m_histograms.at(i);
        const QColor color = qucs_s::ink::on(g->Color);
        QColor fill = color;
        fill.setAlpha(alpha);
        QPen pen(color, g->isSelected ? g->Thick + 2 : std::max(1, g->Thick));
        pen.setJoinStyle(Qt::MiterJoin);
        for (int k = 0; k < h.counts.size(); ++k) {
            if (h.counts.at(k) <= 0.0) continue;
            const QRectF bar(QPointF(X(0.0), Y(h.low + k * h.width)), QPointF(X(h.counts.at(k)), Y(h.low + (k + 1) * h.width)));
            painter->fillRect(bar.normalized(), fill);
            painter->setPen(pen);
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(bar.normalized());
        }
        // Each level's Gaussian: its symbols spread as its mean and sigma say.
        if (gaussians) {
            QPen fit(color.darker(140), std::max(1.5, g->Thick * 1.0));
            fit.setDashPattern({6.0, 3.0});
            painter->setPen(fit);
            for (const eye::Level& l : h.levels) {
                if (!(l.sigma > 0.0)) continue;
                QPolygonF curve;
                for (int j = 0; j <= 200; ++j) {
                    const double v = l.mean + l.sigma * (-5.0 + 10.0 * j / 200.0);
                    const double z = (v - l.mean) / l.sigma;
                    const double pdf = std::exp(-0.5 * z * z) / (l.sigma * std::sqrt(2.0 * kPi));
                    curve << QPointF(X(l.count() * h.width * pdf), Y(v));
                }
                painter->drawPolyline(curve);
            }
        }
    }
    // Each eye's decision threshold: dashed across.
    QPen line(QColor(128, 128, 128), 1, Qt::DashLine);
    painter->setPen(line);
    for (const Histogram& h : m_histograms)
        for (double t : h.thresholds)
            if (std::isfinite(t)) painter->drawLine(QPointF(0.0, Y(t)), QPointF(x2, Y(t)));
    painter->restore();
}

QStringList LevelHistogramDiagram::summary() const
{
    QStringList lines;
    const QString sigma = QString(QChar(0x03C3));
    for (int i = 0; i < m_histograms.size() && i < Graphs.size(); ++i) {
        const Histogram& h = m_histograms.at(i);
        const QString name = nameOf(Graphs.at(i));
        const QString unit = qucs_s::dataset::unitOf(name);
        if (!h.error.isEmpty()) {
            lines << QObject::tr("%1: %2").arg(name, h.error);
            continue;
        }
        lines << QObject::tr("%1: %2 symbols").arg(name).arg(h.symbols);
        for (int k = 0; k < h.levels.size(); ++k) {
            const eye::Level& l = h.levels.at(k);
            if (l.count() == 0) {
                lines << QObject::tr("level %1: no symbol").arg(k + 1);
                continue;
            }
            lines << QObject::tr("level %1: %2, %3 %4 (%5)")
                         .arg(k + 1)
                         .arg(EyeDiagram::engineering(l.mean, unit), sigma, EyeDiagram::engineering(l.sigma, unit))
                         .arg(l.count());
        }
        for (int k = 0; k < h.eyes.size(); ++k) {
            const eye::VoltageBathtub& b = h.eyes.at(k);
            const QString eyeName = h.eyes.size() > 1 ? QObject::tr("eye %1: ").arg(k + 1) : QString();
            if (!b.ok()) {
                lines << eyeName + b.error;
                continue;
            }
            double from = 0, to = 0;
            const QString open = b.opening(ber, &from, &to) ? EyeDiagram::engineering(to - from, unit) : QObject::tr("closed");
            lines << eyeName
                         + (std::isfinite(b.q()) ? QObject::tr("Q %1, BER %2").arg(QString::number(b.q(), 'f', 2), rateText(b.berOfQ()))
                                                 : QObject::tr("Q infinite (no noise)"));
            lines << QObject::tr("%1opening %2 at %3").arg(h.eyes.size() > 1 ? QStringLiteral("   ") : QString(), open, rateText(ber));
        }
    }
    return lines;
}

void LevelHistogramDiagram::paintInFront(QPainter* painter, const Colors& colors)
{
    RectDiagram::paintInFront(painter, colors);
    const QStringList lines = summary();
    if (lines.isEmpty()) return;
    // (y down.) On the right, at the top, the middle or the bottom: where
    // the bars reach the least far across.
    painter->save();
    const QFontMetricsF fm(painter->font());
    double width = 0;
    for (const QString& l : lines) width = std::max(width, fm.horizontalAdvance(l));
    const double margin = 6.0, boxWidth = width + 8, boxHeight = lines.size() * fm.height() + 6;
    const auto reach = [&](double top) {
        // The longest bar between top and top + boxHeight (y down: -y2 .. 0).
        const double from = yAxis.low + (-(top + boxHeight)) / y2 * (yAxis.up - yAxis.low);
        const double to = yAxis.low + (-top) / y2 * (yAxis.up - yAxis.low);
        double longest = 0.0;
        for (const Histogram& h : m_histograms)
            for (int k = 0; k < h.counts.size(); ++k) {
                const double lo = h.low + k * h.width, hi = lo + h.width;
                if (hi > from && lo < to) longest = std::max(longest, h.counts.at(k));
            }
        return longest;
    };
    double top = -y2 + margin;
    double least = reach(top);
    for (const double candidate : {-y2 / 2.0 - boxHeight / 2.0, -boxHeight - margin}) {
        const double r = reach(candidate);
        if (r < least) {
            least = r;
            top = candidate;
        }
    }
    // (Inside the plot: over the axis' numbers it hid them.)
    const QRectF box(std::max(margin, x2 - margin - boxWidth), top, boxWidth, boxHeight);
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

QString LevelHistogramDiagram::extraSaveFields() const
{
    return QStringLiteral(" %1 %2 %3 %4 %5 %6 %7 %8")
        .arg(fieldText(ui), fieldText(start))
        .arg(levels)
        .arg(fieldText(threshold), fieldText(phase))
        .arg(bins)
        .arg(gaussians ? 1 : 0)
        .arg(fieldText(ber));
}

void LevelHistogramDiagram::loadExtraFields(const QStringList& fields)
{
    bool ok = false;
    ui = fieldValue(fields.value(0));
    if (!(ui > 0.0)) ui = eye::NaN;
    start = fieldValue(fields.value(1));
    const int l = fields.value(2).toInt(&ok);
    levels = ok && (l == 2 || l == 4) ? l : 0;
    threshold = fieldValue(fields.value(3));
    const double p = fieldValue(fields.value(4));
    phase = std::isfinite(p) ? std::clamp(p, -0.5, 0.5) : 0.0;
    const int b = fields.value(5).toInt(&ok);
    bins = ok ? std::clamp(b, 0, 1000) : 0;
    gaussians = fields.value(6) != QLatin1String("0");
    const double rate = fieldValue(fields.value(7));
    ber = rate > 0.0 && rate <= 0.01 ? rate : 1e-12;
}
