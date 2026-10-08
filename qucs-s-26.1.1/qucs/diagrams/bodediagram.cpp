/*
 * bodediagram.cpp - the Bode pair: a loop gain's magnitude in dB above its
 * phase, on one frequency axis, with the crossovers and the margins
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "bodediagram.h"

#include "dataset.h"
#include "ink.h"
#include "main.h"
#include "marker.h"
#include "misc.h"

#include <QPainter>

#include <cfloat>
#include <cmath>

namespace {

constexpr double Degrees = 180.0 / 3.14159265358979323846;

// y of a curve at x, straight between its samples; NaN outside.
double valueAt(const QVector<double>& xs, const QVector<double>& ys, double x)
{
    for (int i = 0; i + 1 < xs.size(); ++i) {
        const double a = xs.at(i), b = xs.at(i + 1);
        if ((x - a) * (x - b) > 0.0) continue;
        if (b == a) return ys.at(i);
        // (On a log frequency axis: straight in the logarithm, as drawn.)
        const double t = a > 0 && b > 0 && x > 0 ? std::log(x / a) / std::log(b / a) : (x - a) / (b - a);
        return ys.at(i) + t * (ys.at(i + 1) - ys.at(i));
    }
    return std::nan("");
}

} // namespace

BodeDiagram::BodeDiagram(int cx, int cy) : StackedDiagram(cx, cy)
{
    x2 = 400;
    y2 = 360;
    x3 = x2 + 7;
    Name = "Bode";
    setPaneCount(2);
    xAxis.log = true;   // frequency
    pane(0).left.log = true;   // the magnitude, in dB
    pane(0).left.Units = Axis::dbUnits;
    pane(1).left.log = false;  // the phase, in degrees
    pane(1).left.Units = Axis::NoUnits;
    calcDiagram();
}

Diagram* BodeDiagram::newOne()
{
    return new BodeDiagram();
}

Element* BodeDiagram::info(QString& Name, char*& BitmapFile, bool getNewOne)
{
    Name = QObject::tr("Bode pair");
    BitmapFile = (char*)"bode";
    if (getNewOne) return new BodeDiagram();
    return nullptr;
}

void BodeDiagram::responseOf(const Graph* g, QVector<double>* f, QVector<double>* magnitude, QVector<double>* phase)
{
    f->clear();
    magnitude->clear();
    phase->clear();
    const DataX* xs = g->axis(0);
    if (xs == nullptr || xs->Points == nullptr || g->cPointsY == nullptr) return;
    double last = std::nan("");
    for (int i = 0; i < xs->count; ++i) {
        const double re = g->cPointsY[2 * i], im = g->cPointsY[2 * i + 1];
        double p = std::atan2(im, re) * Degrees;
        // Unwrapped: no jump of more than half a turn from one to the next.
        if (std::isfinite(last)) p += 360.0 * std::round((last - p) / 360.0);
        last = p;
        f->append(xs->Points[i]);
        magnitude->append(std::hypot(re, im));
        phase->append(p);
    }
}

QJsonObject BodeDiagram::marginsOf(const Graph* g)
{
    namespace ds = qucs_s::dataset;
    QVector<double> f, magnitude, phase;
    responseOf(g, &f, &magnitude, &phase);
    ds::Curve c{f, magnitude};
    ds::MeasureOptions o;
    o.phase = phase;
    return {{QStringLiteral("phase margin"), ds::measure(c, QStringLiteral("phase_margin"), o)},
            {QStringLiteral("gain margin"), ds::measure(c, QStringLiteral("gain_margin"), o)}};
}

void BodeDiagram::getAxisLimits(Graph* g)
{
    // Each graph is a loop gain: its magnitude in the upper pane, as it is
    // read (complex), and its phase in the lower one.
    g->pane = 0;
    g->yAxisNo = 0;
    if (g->valuePart != Graph::ValuePart::Auto) {
        g->valuePart = Graph::ValuePart::Auto;
        g->lastLoaded = QDateTime();   // (read again whole)
    }
    StackedDiagram::getAxisLimits(g);
    QVector<double> f, magnitude, phase;
    responseOf(g, &f, &magnitude, &phase);
    Axis& a = pane(1).left;
    ++a.numGraphs;
    for (double p : std::as_const(phase))
        if (std::isfinite(p)) {
            a.min = std::min(a.min, p);
            a.max = std::max(a.max, p);
        }
}

int BodeDiagram::calcDiagram()
{
    if (paneCount() != 2) setPaneCount(2);
    // The phase, scaled by itself: in steps of 45 or 90 degrees, its
    // limits on them (as an automatic scale would not).
    Axis& phase = pane(1).left;
    const bool automatic = phase.autoScale;
    if (automatic && phase.numGraphs > 0 && phase.min <= phase.max && std::isfinite(phase.min) && std::isfinite(phase.max)) {
        const double step = phase.max - phase.min > 270 ? 90.0 : 45.0;
        double low = std::floor(phase.min / step) * step, up = std::ceil(phase.max / step) * step;
        if (up <= low) up = low + step;
        phase.limit_min = low;
        phase.limit_max = up;
        phase.step = step;
        phase.autoScale = false;
    }
    const int valid = StackedDiagram::calcDiagram();
    phase.autoScale = automatic;
    return valid;
}

void BodeDiagram::createAxisLabels()
{
    // The lower pane: the phase (no graph is in it).
    Axis& a = pane(1).left;
    const QString label = a.Label;
    if (label.isEmpty()) a.Label = QObject::tr("phase (°)");
    StackedDiagram::createAxisLabels();
    a.Label = label;
}

QString BodeDiagram::extraMarkerText(Marker const* m) const
{
    QString text = StackedDiagram::extraMarkerText(m);
    if (m->graph() == nullptr || m->varPos().empty()) return text;
    QVector<double> f, magnitude, phase;
    responseOf(m->graph(), &f, &magnitude, &phase);
    const double p = valueAt(f, phase, m->varPos().front());
    if (std::isfinite(p)) text += QObject::tr("\nphase: %1°").arg(QString::number(p, 'f', 1));
    return text;
}

void BodeDiagram::paintBehindGraphs(QPainter* painter)
{
    StackedDiagram::paintBehindGraphs(painter);
    // Each graph's phase in the lower pane (in its coordinates, y up).
    const int b = paneBottom(1), h = paneHeight();
    painter->save();
    painter->setClipRect(QRectF(0, b, x2, h));
    painter->setRenderHint(QPainter::Antialiasing, true);
    const Axis* a = &pane(1).left;
    for (const Graph* g : Graphs) {
        QVector<double> f, magnitude, phase;
        responseOf(g, &f, &magnitude, &phase);
        QPolygonF line;
        for (int i = 0; i < f.size(); ++i) {
            const double y[2] = {phase.at(i), 0};
            float px = 0, py = 0;
            calcCoordinate(&f.at(i), y, nullptr, &px, &py, a);
            line << QPointF(px, py);
        }
        if (g->isSelected) {
            painter->setPen(QPen(Qt::darkGray, g->Thick + 4));
            painter->drawPolyline(line);
        }
        painter->setPen(QPen(qucs_s::ink::on(g->Color), std::max(1, g->Thick), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawPolyline(line);
    }
    painter->restore();
}

void BodeDiagram::paintInFront(QPainter* painter, const Colors& colors)
{
    StackedDiagram::paintInFront(painter, colors);
    if (!margins) return;
    // (In its coordinates, y down.)
    const Axis* magnitudeAxis = &pane(0).left;
    const Axis* phaseAxis = &pane(1).left;
    const auto at = [&](double f, double value, const Axis* a) {
        // A magnitude in dB on the upper pane: back to the ratio it maps.
        double v = value;
        if (a == magnitudeAxis && a->log && a->Units != Axis::NoUnits) v = qucs::db2num(value, a->Units);
        const double y[2] = {v, 0};
        float px = 0, py = 0;
        calcCoordinate(&f, y, nullptr, &px, &py, a);
        return QPointF(px, -py);
    };
    const int h = paneHeight();
    const auto inPane = [&](int i, const QPointF& p) {
        return p.x() >= 0 && p.x() <= x2 && -p.y() >= paneBottom(i) - 0.5 && -p.y() <= paneBottom(i) + h + 0.5;
    };
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    const QColor reference = qucs_s::ink::on(QColor(120, 120, 120));
    // 0 dB above, -180 degrees below: what the crossovers are of.
    painter->setPen(QPen(reference, 0, Qt::DotLine));
    for (const auto& [value, axis, i] : {std::tuple<double, const Axis*, int>{0.0, magnitudeAxis, 0}, {-180.0, phaseAxis, 1}}) {
        const QPointF p = at(xAxis.low > 0 ? xAxis.low : 1.0, value, axis);
        if (-p.y() >= paneBottom(i) && -p.y() <= paneBottom(i) + h) painter->drawLine(QPointF(0, p.y()), QPointF(x2, p.y()));
    }
    for (const Graph* g : Graphs) {
        const QJsonObject m = marginsOf(g);
        const QJsonObject pm = m.value(QStringLiteral("phase margin")).toObject();
        const QJsonObject gm = m.value(QStringLiteral("gain margin")).toObject();
        const QColor ink = qucs_s::ink::on(g->Color);
        if (!pm.contains(QStringLiteral("error"))) {
            const double fc = pm.value(QStringLiteral("gain crossover")).toDouble();
            const double ph = pm.value(QStringLiteral("phase there")).toDouble();
            const QPointF top = at(fc, 0.0, magnitudeAxis), p = at(fc, ph, phaseAxis), ref = at(fc, -180.0, phaseAxis);
            painter->setPen(QPen(ink, 0, Qt::DashLine));
            for (int i = 0; i < 2; ++i) painter->drawLine(QPointF(top.x(), -(paneBottom(i) + h)), QPointF(top.x(), -paneBottom(i)));
            painter->setPen(QPen(ink, 2));
            painter->setBrush(ink);
            if (inPane(0, top)) painter->drawEllipse(top, 3, 3);
            if (inPane(1, p) || inPane(1, ref)) {
                const double clipTop = -(paneBottom(1) + h), clipBottom = -paneBottom(1);
                painter->drawLine(QPointF(p.x(), std::clamp(p.y(), clipTop, clipBottom)), QPointF(ref.x(), std::clamp(ref.y(), clipTop, clipBottom)));
                painter->drawText(QPointF(p.x() + 5, std::clamp((p.y() + ref.y()) / 2, clipTop + 12, clipBottom - 4)),
                                  QObject::tr("PM %1°").arg(QString::number(pm.value(QStringLiteral("value")).toDouble(), 'f', 1)));
            }
        }
        if (!gm.contains(QStringLiteral("error"))) {
            const double f180 = gm.value(QStringLiteral("phase crossover")).toDouble();
            const double gain = gm.value(QStringLiteral("gain there, dB")).toDouble();
            const QPointF zero = at(f180, 0.0, magnitudeAxis), g0 = at(f180, gain, magnitudeAxis), ph = at(f180, -180.0, phaseAxis);
            painter->setPen(QPen(ink, 0, Qt::DashDotLine));
            for (int i = 0; i < 2; ++i) painter->drawLine(QPointF(zero.x(), -(paneBottom(i) + h)), QPointF(zero.x(), -paneBottom(i)));
            painter->setPen(QPen(ink, 2));
            painter->setBrush(ink);
            if (inPane(1, ph)) painter->drawEllipse(ph, 3, 3);
            const double clipTop = -(paneBottom(0) + h), clipBottom = -paneBottom(0);
            painter->drawLine(QPointF(zero.x(), std::clamp(zero.y(), clipTop, clipBottom)), QPointF(g0.x(), std::clamp(g0.y(), clipTop, clipBottom)));
            painter->drawText(QPointF(zero.x() + 5, std::clamp((zero.y() + g0.y()) / 2, clipTop + 12, clipBottom - 4)),
                              QObject::tr("GM %1 dB").arg(QString::number(gm.value(QStringLiteral("value")).toDouble(), 'f', 1)));
        }
    }
    painter->restore();
}

QString BodeDiagram::extraSaveFields() const
{
    return StackedDiagram::extraSaveFields() + QStringLiteral(" %1").arg(margins ? 1 : 0);
}

void BodeDiagram::loadExtraFields(const QStringList& fields)
{
    StackedDiagram::loadExtraFields(fields);
    // After the panes': whether the margins are marked.
    bool ok = false;
    const int n = fields.value(0).toInt(&ok);
    const QString m = fields.value(1 + 16 * (ok ? n : 2));
    margins = m != QLatin1String("0");
    if (paneCount() != 2) setPaneCount(2);
}
