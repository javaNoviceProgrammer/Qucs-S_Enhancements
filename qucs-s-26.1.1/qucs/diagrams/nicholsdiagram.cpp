/*
 * nicholsdiagram.cpp - the Nichols chart: a loop gain's open-loop gain in
 * dB against its phase, over the closed loop's M and N contours
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "nicholsdiagram.h"

#include "ink.h"
#include "main.h"
#include "marker.h"

#include <QPainter>

#include <cfloat>
#include <cmath>
#include <complex>

namespace {

constexpr double Pi = 3.14159265358979323846;
constexpr double Degrees = 180.0 / Pi;

// A phase in degrees within -360 to 0 (as a loop's usually is).
double inWindow(double degrees)
{
    double p = std::fmod(degrees, 360.0);
    if (p > 0) p -= 360.0;
    if (p <= -360.0) p += 360.0;
    return p;
}

// G = C / (1 - C) as (phase in the window, gain dB).
QPointF openLoop(std::complex<double> c)
{
    const std::complex<double> g = c / (1.0 - c);
    return QPointF(inWindow(std::arg(g) * Degrees), 20.0 * std::log10(std::abs(g)));
}

// \a points with a NaN point where the phase jumps (wraps): pieces drawn
// apart.
QPolygonF broken(const QVector<QPointF>& points)
{
    QPolygonF out;
    for (int i = 0; i < points.size(); ++i) {
        const QPointF p = points.at(i);
        if (!std::isfinite(p.x()) || !std::isfinite(p.y())) {
            if (!out.isEmpty() && std::isfinite(out.last().x())) out << QPointF(NAN, NAN);
            continue;
        }
        if (!out.isEmpty() && std::isfinite(out.last().x()) && std::abs(p.x() - out.last().x()) > 180.0) out << QPointF(NAN, NAN);
        out << p;
    }
    return out;
}

} // namespace

NicholsDiagram::NicholsDiagram(int cx, int cy) : RectDiagram(cx, cy)
{
    x2 = 360;
    y2 = 300;
    x3 = x2 + 7;
    Name = "Nichols";
    calcDiagram();
}

Diagram* NicholsDiagram::newOne()
{
    return new NicholsDiagram();
}

Element* NicholsDiagram::info(QString& Name, char*& BitmapFile, bool getNewOne)
{
    Name = QObject::tr("Nichols chart");
    BitmapFile = (char*)"nichols";
    if (getNewOne) return new NicholsDiagram();
    return nullptr;
}

void NicholsDiagram::pointsOf(const Graph* g, QVector<double>* phase, QVector<double>* gain)
{
    phase->clear();
    gain->clear();
    const DataX* xs = g->axis(0);
    if (xs == nullptr || g->cPointsY == nullptr) return;
    const int n = xs->count * g->countY;
    double last = std::nan("");
    for (int i = 0; i < n; ++i) {
        const double re = g->cPointsY[2 * i], im = g->cPointsY[2 * i + 1];
        double p = std::atan2(im, re) * Degrees;
        // Each curve from its first sample (within -360 to 0), unwrapped.
        if (i % xs->count == 0) last = std::nan("");
        p = std::isfinite(last) ? p + 360.0 * std::round((last - p) / 360.0) : inWindow(p);
        last = p;
        phase->append(p);
        gain->append(20.0 * std::log10(std::hypot(re, im)));
    }
}

QPolygonF NicholsDiagram::mContour(double mDb)
{
    const double m = std::pow(10.0, mDb / 20.0);
    QVector<QPointF> points;
    for (int k = 0; k <= 720; ++k) {
        const double t = -Pi + 2 * Pi * k / 720.0;
        const std::complex<double> c = std::polar(m, t);
        if (std::abs(1.0 - c) < 1e-9) {
            points << QPointF(NAN, NAN);
            continue;
        }
        points << openLoop(c);
    }
    return broken(points);
}

QPolygonF NicholsDiagram::nContour(double nDegrees)
{
    QVector<QPointF> points;
    for (int k = 0; k <= 600; ++k) {
        const double r = std::pow(10.0, -8.0 + 11.0 * k / 600.0);   // (down to -160 dB)
        const std::complex<double> c = std::polar(r, nDegrees / Degrees);
        if (std::abs(1.0 - c) < 1e-9) {
            points << QPointF(NAN, NAN);
            continue;
        }
        points << openLoop(c);
    }
    return broken(points);
}

void NicholsDiagram::getAxisLimits(Graph* g)
{
    QVector<double> phase, gain;
    pointsOf(g, &phase, &gain);
    Axis* pa = graphAxis(g);
    ++pa->numGraphs;
    for (int i = 0; i < phase.size(); ++i) {
        if (!std::isfinite(phase.at(i)) || !std::isfinite(gain.at(i))) continue;
        xAxis.min = std::min(xAxis.min, phase.at(i));
        xAxis.max = std::max(xAxis.max, phase.at(i));
        pa->min = std::min(pa->min, gain.at(i));
        pa->max = std::max(pa->max, gain.at(i));
    }
}

int NicholsDiagram::calcDiagram()
{
    // The critical point in sight; the phase in steps of 45 degrees (90
    // for a wide one) - from the graphs, the same at every layout.
    double lo = -180.0, hi = -180.0, bottom = 0.0, top = 0.0;
    for (const Graph* g : Graphs) {
        QVector<double> phase, gain;
        pointsOf(g, &phase, &gain);
        for (int i = 0; i < phase.size(); ++i) {
            if (!std::isfinite(phase.at(i)) || !std::isfinite(gain.at(i))) continue;
            lo = std::min(lo, phase.at(i));
            hi = std::max(hi, phase.at(i));
            bottom = std::min(bottom, gain.at(i));
            top = std::max(top, gain.at(i));
        }
    }
    xAxis.log = yAxis.log = false;
    yAxis.min = bottom;
    yAxis.max = top;
    const bool automatic = xAxis.autoScale;
    if (automatic) {
        const double step = hi - lo > 360 ? 90.0 : 45.0;
        xAxis.limit_min = std::floor(lo / step) * step;
        xAxis.limit_max = std::ceil(hi / step) * step;
        if (xAxis.limit_max <= xAxis.limit_min) xAxis.limit_max = xAxis.limit_min + step;
        xAxis.step = step;
        xAxis.autoScale = false;
    }
    const int valid = RectDiagram::calcDiagram();
    xAxis.autoScale = automatic;
    return valid;
}

void NicholsDiagram::calcCoordinate(const double*, const double* yD, const double*, float* px, float* py, Axis const* pa) const
{
    // (phase, gain dB) of the value: a traced sample's unwrapped phase.
    double phase = inWindow(std::atan2(yD[1], yD[0]) * Degrees);
    if (m_tracing && m_tracing->cPointsY && m_tracing->axis(0)) {
        const ptrdiff_t i = (yD - m_tracing->cPointsY) / 2;
        if (i >= 0 && i < m_phase.size()) phase = m_phase.at(i);
    }
    const double gain = 20.0 * std::log10(std::hypot(yD[0], yD[1]));
    *px = float((phase - xAxis.low) / (xAxis.up - xAxis.low) * double(x2));
    *py = float((gain - pa->low) / (pa->up - pa->low) * double(y2));
    if (!std::isfinite(*px)) *px = 0.0;
    if (!std::isfinite(*py)) *py = gain < pa->low ? -1e5f : 0.0f;
}

void NicholsDiagram::calcData(Graph* g)
{
    QVector<double> gain;
    pointsOf(g, &m_phase, &gain);
    m_tracing = g;
    RectDiagram::calcData(g);
    m_tracing = nullptr;
    m_phase.clear();
}

void NicholsDiagram::createAxisLabels()
{
    const QString x = xAxis.Label, y = yAxis.Label;
    if (x.isEmpty()) xAxis.Label = QObject::tr("open-loop phase (°)");
    if (y.isEmpty()) yAxis.Label = QObject::tr("open-loop gain (dB)");
    RectDiagram::createAxisLabels();
    xAxis.Label = x;
    yAxis.Label = y;
}

QString NicholsDiagram::extraMarkerText(Marker const* m) const
{
    if (m->graph() == nullptr || m->varPos().empty()) return {};
    std::vector<double> at = m->varPos();
    const auto p = m->graph()->findSample(at);
    const std::complex<double> g(p.first, p.second);
    const std::complex<double> closed = g / (1.0 + g);
    return QObject::tr("\ngain: %1 dB, phase: %2°\nclosed loop: %3 dB, %4°")
        .arg(QString::number(20 * std::log10(std::abs(g)), 'f', 2), QString::number(inWindow(std::arg(g) * Degrees), 'f', 1),
             QString::number(20 * std::log10(std::abs(closed)), 'f', 2), QString::number(std::arg(closed) * Degrees, 'f', 1));
}

void NicholsDiagram::paintBehindGraphs(QPainter* painter)
{
    if (!grid) return;
    // (In its coordinates: origin at the lower left corner, y up.)
    painter->save();
    painter->setClipRect(QRectF(0, 0, x2, y2));
    painter->setRenderHint(QPainter::Antialiasing, true);
    const auto at = [&](double phase, double gain) {
        return QPointF((phase - xAxis.low) / (xAxis.up - xAxis.low) * x2, (gain - yAxis.low) / (yAxis.up - yAxis.low) * y2);
    };
    QFont small = painter->font();
    if (small.pointSizeF() > 0) small.setPointSizeF(small.pointSizeF() * 0.75);
    painter->setFont(small);
    const QColor m = qucs_s::ink::on(QColor(70, 120, 200)), n = qucs_s::ink::on(QColor(200, 120, 70));
    const int first = int(std::floor(xAxis.low / 360.0)), last = int(std::ceil(xAxis.up / 360.0)) + 1;
    // The labels: an M contour's at its rightmost point in sight, an N
    // contour's at its lowest; one that would fall on another left out.
    const QFontMetricsF fm(small);
    QList<QRectF> placed;
    const auto label = [&](const QPointF& q, const QString& text, const QColor& colour) {
        const QSizeF size(fm.horizontalAdvance(text) + 2, fm.height());
        QRectF box(QPointF(q.x() + 2, q.y() + 1), size);
        box.moveLeft(std::clamp(box.left(), 1.0, double(x2) - box.width() - 1));
        box.moveTop(std::clamp(box.top(), 1.0, double(y2) - box.height() - 1));
        for (const QRectF& r : std::as_const(placed))
            if (r.adjusted(-2, -2, 2, 2).intersects(box)) return;
        placed << box;
        painter->save();
        painter->translate(box.left(), box.bottom());
        painter->scale(1, -1);
        painter->setPen(colour);
        painter->drawText(QPointF(0, size.height() - fm.descent()), text);
        painter->restore();
    };
    const auto draw = [&](const QPolygonF& contour, const QString& text, const QColor& colour, bool lowest) {
        painter->setPen(QPen(colour, 0, Qt::DashLine));
        QPointF best(NAN, NAN);
        for (int k = first; k <= last; ++k) {
            QPolygonF piece;
            const auto flush = [&] {
                if (piece.size() > 1) painter->drawPolyline(piece);
                piece.clear();
            };
            for (const QPointF& p : contour) {
                if (!std::isfinite(p.x())) {
                    flush();
                    continue;
                }
                const QPointF q = at(p.x() + 360.0 * k, p.y());
                piece << q;
                if (q.x() < 2 || q.x() > x2 - 2 || q.y() < 2 || q.y() > y2 - 2) continue;
                if (!std::isfinite(best.x()) || (lowest ? q.y() < best.y() : q.x() > best.x())) best = q;
            }
            flush();
        }
        if (std::isfinite(best.x())) label(best, text, colour);
    };
    for (const double dB : {6.0, 3.0, 1.0, 0.5, 0.25, 0.0, -1.0, -3.0, -6.0, -12.0, -20.0})
        draw(mContour(dB), QStringLiteral("%1 dB").arg(dB), m, false);
    for (const double deg : {-1.0, -5.0, -10.0, -20.0, -30.0, -60.0, -90.0, -120.0, -150.0})
        draw(nContour(deg), QStringLiteral("%1°").arg(deg), n, true);
    painter->restore();
}

void NicholsDiagram::paintInFront(QPainter* painter, const Colors& colors)
{
    RectDiagram::paintInFront(painter, colors);
    // The critical point, -180 degrees at 0 dB (a turn either way too).
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QPen(qucs_s::ink::on(QColor(210, 30, 30)), 2));
    for (int k = int(std::floor(xAxis.low / 360.0)) - 1; k <= int(std::ceil(xAxis.up / 360.0)) + 1; ++k) {
        const double phase = -180.0 + 360.0 * k;
        if (phase < xAxis.low || phase > xAxis.up || 0.0 < yAxis.low || 0.0 > yAxis.up) continue;
        const QPointF p((phase - xAxis.low) / (xAxis.up - xAxis.low) * x2, -(0.0 - yAxis.low) / (yAxis.up - yAxis.low) * y2);
        painter->drawLine(p + QPointF(-6, 0), p + QPointF(6, 0));
        painter->drawLine(p + QPointF(0, -6), p + QPointF(0, 6));
        painter->drawEllipse(p, 4.0, 4.0);
    }
    painter->restore();
}

QString NicholsDiagram::extraSaveFields() const
{
    return QStringLiteral(" %1").arg(grid ? 1 : 0);
}

void NicholsDiagram::loadExtraFields(const QStringList& fields)
{
    bool ok = false;
    const int g = fields.value(0).toInt(&ok);
    grid = !ok || g != 0;
}
