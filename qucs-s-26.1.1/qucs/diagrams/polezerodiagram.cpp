/*
 * polezerodiagram.cpp - the pole-zero map: a pole-zero analysis' roots in
 * the s-plane
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "polezerodiagram.h"

#include "ink.h"
#include "main.h"
#include "marker.h"
#include "misc.h"

#include <QPainter>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace {

constexpr double Pi = 3.14159265358979323846;

// A step of 1, 2 or 5 times a power of ten.
double niceStep(double span)
{
    if (!(span > 0) || !std::isfinite(span)) return 1.0;
    const double power = std::pow(10.0, std::floor(std::log10(span)));
    const double f = span / power;
    return (f < 1.5 ? 1 : f < 3.5 ? 2 : f < 7.5 ? 5 : 10) * power;
}

// \a v with three significant digits and an SI prefix: "1.01 Mrad/s".
QString engineering(double v, const QString& unit)
{
    static const char* const prefixes[] = {"f", "p", "n", "\u00B5", "m", "", "k", "M", "G", "T"};
    if (v == 0.0 || !std::isfinite(v)) return QString::number(v) + QLatin1Char(' ') + unit;
    const int e = std::clamp(int(std::floor(std::log10(std::fabs(v)) / 3.0)), -5, 4);
    const double m = v / std::pow(1000.0, e);
    return QString::number(m, 'g', 3) + QLatin1Char(' ') + QString::fromUtf8(prefixes[e + 5]) + unit;
}

} // namespace

PoleZeroDiagram::PoleZeroDiagram(int cx, int cy) : RectDiagram(cx, cy)
{
    x2 = 300;
    y2 = 300;
    x3 = x2 + 7;
    Name = "PoleZero";
    xAxis.GridOn = yAxis.GridOn = true;
    calcDiagram();
}

Diagram* PoleZeroDiagram::newOne()
{
    return new PoleZeroDiagram();
}

Element* PoleZeroDiagram::info(QString& Name, char*& BitmapFile, bool getNewOne)
{
    Name = QObject::tr("Pole-zero map");
    BitmapFile = (char*)"polezero";
    if (getNewOne) return new PoleZeroDiagram();
    return nullptr;
}

double PoleZeroDiagram::Root::wn() const
{
    return std::hypot(re, im);
}

double PoleZeroDiagram::Root::zeta() const
{
    const double w = wn();
    return w > 0 ? -re / w : std::nan("");
}

double PoleZeroDiagram::Root::q() const
{
    const double z = zeta();
    return z > 0 ? 1.0 / (2.0 * z) : INFINITY;
}

QList<PoleZeroDiagram::Root> PoleZeroDiagram::rootsOf(const Graph* g)
{
    QList<Root> roots;
    const DataX* xs = g->axis(0);
    if (xs == nullptr || g->cPointsY == nullptr) return roots;
    const int n = xs->count * g->countY;
    for (int i = 0; i < n; ++i) {
        const double re = g->cPointsY[2 * i], im = g->cPointsY[2 * i + 1];
        if (std::isfinite(re) && std::isfinite(im)) roots << Root{re, im};
    }
    return roots;
}

bool PoleZeroDiagram::isZero(const Graph* g)
{
    const QString name = g->Var.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), -1).toLower();
    return name.startsWith(QLatin1String("zero"));
}

QString PoleZeroDiagram::rootText(const Root& r)
{
    QString text = QObject::tr("ωn %1 (f %2), ζ %3")
                       .arg(engineering(r.wn(), QStringLiteral("rad/s")), engineering(r.wn() / (2 * Pi), QStringLiteral("Hz")))
                       .arg(std::isfinite(r.zeta()) ? QString::number(r.zeta(), 'g', 3) : QStringLiteral("-"));
    if (std::isfinite(r.q())) text += QObject::tr(", Q %1").arg(QString::number(r.q(), 'g', 3));
    if (r.re > 0) text += QObject::tr(" - in the right half-plane");
    else if (r.re == 0) text += QObject::tr(" - on the imaginary axis");
    return text;
}

void PoleZeroDiagram::getAxisLimits(Graph* g)
{
    // x the real part, y the imaginary (as a locus curve's).
    Axis* pa = graphAxis(g);
    ++pa->numGraphs;
    for (const Root& r : rootsOf(g)) {
        xAxis.min = std::min(xAxis.min, r.re);
        xAxis.max = std::max(xAxis.max, r.re);
        pa->min = std::min(pa->min, r.im);
        pa->max = std::max(pa->max, r.im);
    }
}

int PoleZeroDiagram::calcDiagram()
{
    // The j omega axis in sight, and the plane symmetric about the real
    // axis (conjugate pairs), each with a margin - from the roots, so that
    // it comes out the same however often it is laid out.
    double lo = 0.0, hi = 0.0, y = 0.0;
    bool any = false;
    for (const Graph* g : Graphs)
        for (const Root& r : rootsOf(g)) {
            lo = std::min(lo, r.re);
            hi = std::max(hi, r.re);
            y = std::max(y, std::fabs(r.im));
            any = true;
        }
    if (any) {
        const double reach = std::max({-lo, hi, y, 1e-30});
        const double margin = 0.15 * std::max(hi - lo, reach * 0.2);
        xAxis.min = lo - margin;
        xAxis.max = hi + margin;
        if (y == 0.0) y = 0.5 * (xAxis.max - xAxis.min);
        yAxis.min = -1.15 * y;
        yAxis.max = 1.15 * y;
    }
    xAxis.log = yAxis.log = false;
    return RectDiagram::calcDiagram();
}

void PoleZeroDiagram::calcCoordinate(const double*, const double* yD, const double*, float* px, float* py, Axis const* pa) const
{
    // (re, im): x the real part, y the imaginary - whatever its index.
    *px = float((yD[0] - xAxis.low) / (xAxis.up - xAxis.low) * double(x2));
    *py = float((yD[1] - pa->low) / (pa->up - pa->low) * double(y2));
    if (!std::isfinite(*px)) *px = 0.0;
    if (!std::isfinite(*py)) *py = 0.0;
}

void PoleZeroDiagram::calcData(Graph* g)
{
    // Its roots as points (each where it is, none joined): what a click
    // on one finds, a marker set on it. Drawn as crosses and circles below.
    const graphstyle_t style = g->Style;
    g->Style = GRAPHSTYLE_CIRCLE;
    RectDiagram::calcData(g);
    g->Style = style;
}

void PoleZeroDiagram::createAxisLabels()
{
    // sigma along x, j omega up y - unless the axes have labels of their own.
    const QString x = xAxis.Label, y = yAxis.Label;
    if (x.isEmpty()) xAxis.Label = QObject::tr("σ (rad/s)");
    if (y.isEmpty() && zAxis.numGraphs == 0) yAxis.Label = QObject::tr("jω (rad/s)");
    RectDiagram::createAxisLabels();
    xAxis.Label = x;
    yAxis.Label = y;
}

QString PoleZeroDiagram::extraMarkerText(Marker const* m) const
{
    if (m->graph() == nullptr || m->varPos().empty()) return {};
    // The root marked: the sample its index is at.
    std::vector<double> at = m->varPos();
    const auto p = m->graph()->findSample(at);
    return QStringLiteral("\n") + rootText(Root{p.first, p.second}).replace(QStringLiteral(", "), QStringLiteral("\n"));
}

void PoleZeroDiagram::paintBehindGraphs(QPainter* painter)
{
    // (In its coordinates: origin at the lower left corner, y up.)
    painter->save();
    painter->setClipRect(QRectF(0, 0, x2, y2));
    painter->setRenderHint(QPainter::Antialiasing, true);
    const auto at = [&](double re, double im) {
        const double y[2] = {re, im};
        float px = 0, py = 0;
        calcCoordinate(nullptr, y, nullptr, &px, &py, &yAxis);
        return QPointF(px, py);
    };
    const QPointF origin = at(0, 0);

    // The right half-plane: unstable, lightly shaded.
    if (origin.x() < x2) painter->fillRect(QRectF(QPointF(std::max(0.0, origin.x()), 0), QPointF(x2, y2)), QColor(220, 60, 60, 22));

    const QColor guide = qucs_s::ink::on(QColor(110, 110, 140));
    if (guides && xAxis.low < 0) {
        QFont small = painter->font();
        if (small.pointSizeF() > 0) small.setPointSizeF(small.pointSizeF() * 0.8);
        painter->setFont(small);
        // Constant damping: rays from the origin into the left half-plane,
        // each labelled where it leaves the plot (labels that would fall
        // on one already there left out).
        const double sx = double(x2) / (xAxis.up - xAxis.low), sy = double(y2) / (yAxis.up - yAxis.low);
        const QFontMetricsF fm(small);
        QList<QRectF> labels;
        const auto label = [&](const QPointF& p, const QString& text) {
            // (Upright, its box kept in the plot.)
            const QSizeF size(fm.horizontalAdvance(text) + 2, fm.height());
            QRectF box(QPointF(p.x() + 2, p.y() - size.height() - 1), size);
            box.moveLeft(std::clamp(box.left(), 1.0, double(x2) - box.width() - 1));
            box.moveTop(std::clamp(box.top(), 1.0, double(y2) - box.height() - 1));
            for (const QRectF& r : std::as_const(labels))
                if (r.adjusted(-2, -2, 2, 2).intersects(box)) return;
            labels << box;
            painter->save();
            painter->translate(box.left(), box.bottom());
            painter->scale(1, -1);
            painter->drawText(QPointF(0, size.height() - fm.descent()), text);
            painter->restore();
        };
        painter->setPen(QPen(guide, 0, Qt::DotLine));
        for (const double z : {0.1, 0.2, 0.4, 0.6, 0.8, 0.95}) {
            const double s = std::sqrt(1 - z * z);
            const QPointF dir(-z * sx, s * sy);   // (the ray on the screen, upper half)
            // Where it leaves the plot: the left edge or the top.
            const double tLeft = dir.x() < 0 ? -origin.x() / dir.x() : INFINITY;
            const double tTop = dir.y() > 0 ? (y2 - origin.y()) / dir.y() : INFINITY;
            const double t = std::min(tLeft, tTop);
            if (!std::isfinite(t) || t <= 0) continue;
            const QPointF exit = origin + t * dir;
            painter->drawLine(origin, exit);
            painter->drawLine(origin, origin + t * QPointF(dir.x(), -dir.y()));
            label(exit, QStringLiteral("ζ %1").arg(z));
        }
        // Constant natural frequency: half circles about the origin, in
        // the left half-plane, labelled where they cross the real axis.
        const double step = niceStep(-xAxis.low / 3.5);
        painter->setPen(QPen(guide, 0, Qt::DashLine));
        for (int k = 1; k * step < -xAxis.low && k < 20; ++k) {
            const double r = k * step;
            const QRectF box(origin.x() - r * sx, origin.y() - r * sy, 2 * r * sx, 2 * r * sy);
            painter->drawArc(box, 90 * 16, 180 * 16);
            label(QPointF(origin.x() - r * sx, origin.y() + 2), QStringLiteral("ωn %1").arg(numberText(r, step)));
        }
    }
    // The j omega axis and the real axis.
    painter->setPen(QPen(qucs_s::ink::on(QColor(60, 60, 60)), 1.2));
    painter->drawLine(QPointF(origin.x(), 0), QPointF(origin.x(), y2));
    painter->setPen(QPen(qucs_s::ink::on(QColor(60, 60, 60)), 0));
    painter->drawLine(QPointF(0, origin.y()), QPointF(x2, origin.y()));

    // The roots: crosses for poles, circles for zeros, in their graph's
    // colour (a selected graph's under a grey halo).
    for (const Graph* g : Graphs) {
        const bool zero = isZero(g);
        const double size = 5.0 + g->Thick;
        for (const Root& r : rootsOf(g)) {
            const QPointF p = at(r.re, r.im);
            for (int pass = g->isSelected ? 0 : 1; pass < 2; ++pass) {
                const QColor c = pass == 0 ? QColor(Qt::darkGray) : qucs_s::ink::on(g->Color);
                painter->setPen(QPen(c, std::max(1.5, double(g->Thick)) + (pass == 0 ? 3 : 0), Qt::SolidLine, Qt::RoundCap));
                painter->setBrush(Qt::NoBrush);
                if (zero) {
                    painter->drawEllipse(p, size, size);
                } else {
                    painter->drawLine(p + QPointF(-size, -size), p + QPointF(size, size));
                    painter->drawLine(p + QPointF(-size, size), p + QPointF(size, -size));
                }
            }
        }
    }
    painter->restore();
}

QString PoleZeroDiagram::extraSaveFields() const
{
    return QStringLiteral(" %1").arg(guides ? 1 : 0);
}

void PoleZeroDiagram::loadExtraFields(const QStringList& fields)
{
    bool ok = false;
    const int g = fields.value(0).toInt(&ok);
    guides = !ok || g != 0;
}
