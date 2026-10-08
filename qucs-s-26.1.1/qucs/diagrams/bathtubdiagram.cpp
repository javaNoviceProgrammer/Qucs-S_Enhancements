/*
 * bathtubdiagram.cpp - the bathtub curve: a serial data signal's bit error
 * rate against the sampling instant, beside its eye
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "bathtubdiagram.h"

#include "dataset.h"
#include "ink.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace eye = qucs_s::eye;

namespace {

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

// The sampling instant, as the voltage tub says it: "the centre", "+0.25 UI".
QString phaseText(double phase)
{
    return phase == 0.0 ? QObject::tr("the eye's centre")
                        : QObject::tr("%1 UI from the eye's centre").arg((phase > 0 ? QStringLiteral("+") : QString()) + QString::number(phase, 'g', 3));
}

} // namespace

BathtubDiagram::BathtubDiagram(int cx, int cy)
    : RectDiagram(cx, cy), ui(eye::NaN), start(eye::NaN), threshold(eye::NaN), floor(eye::NaN)
{
    x2 = 400;
    y2 = 260;
    x3 = x2 + 7;
    Name = "Bathtub";
    // (Automatic: each axis in its own - notationOf().)
    notation = qucs_s::numberformat::Notation::Automatic;
    calcDiagram();
}

Diagram* BathtubDiagram::newOne()
{
    return new BathtubDiagram();
}

Element* BathtubDiagram::info(QString& Name, char*& BitmapFile, bool getNewOne)
{
    Name = QObject::tr("Bathtub Curve");
    BitmapFile = (char*)"bathtub";
    if (getNewOne) return new BathtubDiagram();
    return nullptr;
}

double BathtubDiagram::floorRate() const
{
    return std::isfinite(floor) && floor > 0.0 ? floor : ber * 1e-4;
}

qucs_s::numberformat::Notation BathtubDiagram::notationOf(const Axis* axis) const
{
    using qucs_s::numberformat::Notation;
    if (notation != Notation::Automatic || notationDecimals >= 0) return notation;
    // (On its side, the thresholds in the signal's unit as any axis has it.)
    if (axis == &xAxis) return direction == Voltage ? Notation::Automatic : Notation::Decimal;
    return Notation::Power;
}

QList<Diagram::Part> BathtubDiagram::themeParts() const
{
    QList<Part> parts = RectDiagram::themeParts();
    parts.removeAll(Part::RightAxis);
    return parts;
}

void BathtubDiagram::analyse()
{
    eye::Options o;
    o.start = start;
    o.threshold = threshold;
    m_folding = EyeDiagram::foldingOf(this, ui, levels, o);
    m_tubs.clear();
    m_volts.clear();
    if (direction == Voltage) {
        // Each graph sampled at the instant, each eye's levels on either side.
        for (int i = 0; i < m_folding.results.size(); ++i) {
            const eye::Result& r = m_folding.results.at(i);
            QList<eye::VoltageBathtub> tubs;
            if (!r.ok()) {
                eye::VoltageBathtub none;
                none.error = r.error;
                tubs << none;
            } else {
                const QVector<double> sampled = eye::sampledAt(m_folding.curves.value(i), r, phase);
                for (int k = 0; k < r.eyes.size(); ++k) tubs << eye::voltageBathtubOf(r, k, sampled, phase);
            }
            m_volts << tubs;
        }
        return;
    }
    for (const eye::Result& r : std::as_const(m_folding.results)) {
        QList<eye::Bathtub> tubs;
        if (!r.ok()) {
            eye::Bathtub none;
            none.error = r.error;
            tubs << none;
        } else {
            for (int k = 0; k < r.eyes.size(); ++k) tubs << eye::bathtubOf(r, k);
        }
        m_tubs << tubs;
    }
}

void BathtubDiagram::getAxisLimits(Graph* g)
{
    g->yAxisNo = 0;   // one axis, the rate's
    ++yAxis.numGraphs;
}

int BathtubDiagram::calcDiagram()
{
    xAxis.log = false;
    yAxis.log = true;
    analyse();
    // Across: from the left wall to the right (in UI from the eye's
    // centre); up: from the floor to 1.
    double lo = eye::NaN, hi = eye::NaN;
    for (const QList<eye::Bathtub>& tubs : std::as_const(m_tubs))
        for (const eye::Bathtub& b : tubs)
            if (b.ok()) {
                lo = std::isfinite(lo) ? std::min(lo, b.left) : b.left;
                hi = std::isfinite(hi) ? std::max(hi, b.left + 1.0) : b.left + 1.0;
            }
    // On its side: from the lowest level to the highest (each level the
    // wall of the eyes beside it).
    for (const QList<eye::VoltageBathtub>& tubs : std::as_const(m_volts))
        for (const eye::VoltageBathtub& b : tubs)
            if (b.ok()) {
                lo = std::isfinite(lo) ? std::min(lo, b.low.mean) : b.low.mean;
                hi = std::isfinite(hi) ? std::max(hi, b.high.mean) : b.high.mean;
            }
    xAxis.min = std::isfinite(lo) ? lo : direction == Voltage ? 0.0 : -0.5;
    xAxis.max = std::isfinite(hi) && hi > lo ? hi : direction == Voltage ? 1.0 : 0.5;
    yAxis.min = floorRate();
    yAxis.max = 1.0;
    return RectDiagram::calcDiagram();
}

void BathtubDiagram::createAxisLabels()
{
    const QString x = xAxis.Label, y = yAxis.Label;
    if (x.isEmpty())
        xAxis.Label = direction == Voltage ? QObject::tr("decision threshold, sampling at %1").arg(phaseText(phase))
                                           : QObject::tr("sampling instant (UI from the eye's centre)");
    if (y.isEmpty()) yAxis.Label = QObject::tr("bit error rate");
    RectDiagram::createAxisLabels();
    xAxis.Label = x;
    yAxis.Label = y;
}

void BathtubDiagram::paintBehindGraphs(QPainter* painter)
{
    if (direction == Voltage) {
        paintVoltage(painter);
        return;
    }
    // (In its coordinates: origin at the lower left corner, y up.)
    painter->save();
    painter->setClipRect(QRectF(0, 0, x2, y2));
    painter->setRenderHint(QPainter::Antialiasing, true);
    const double bottom = floorRate() / 10.0;   // below the axis: drawn out of the frame
    const auto at = [&](double phase, double rate) {
        const double y[2] = {std::max(rate, bottom), 0};
        float px = 0, py = 0;
        calcCoordinate(&phase, y, nullptr, &px, &py, &yAxis);
        return QPointF(px, py);
    };
    // The target rate: dashed across.
    painter->setPen(QPen(QColor(128, 128, 128), 1, Qt::DashLine));
    painter->drawLine(at(xAxis.low, ber), at(xAxis.up, ber));
    for (int i = 0; i < m_tubs.size() && i < Graphs.size(); ++i) {
        const Graph* g = Graphs.at(i);
        const QColor ink = qucs_s::ink::on(g->Color);
        const QList<eye::Bathtub>& tubs = m_tubs.at(i);
        int narrowest = -1;
        double narrowestWidth = 0;
        for (int k = 0; k < tubs.size(); ++k) {
            const eye::Bathtub& b = tubs.at(k);
            if (!b.ok()) continue;
            // The crossings counted: steps, thin.
            if (measured && !b.offsets.isEmpty()) {
                QVector<double> breaks;
                for (double o : b.offsets) breaks << b.left + o << b.left + 1.0 + o;
                breaks << xAxis.low << xAxis.up;
                std::sort(breaks.begin(), breaks.end());
                QColor light = ink;
                light.setAlpha(150);
                painter->setPen(QPen(light, 1));
                QPolygonF steps;
                for (int j = 0; j + 1 < breaks.size(); ++j) {
                    if (breaks.at(j + 1) <= breaks.at(j)) continue;
                    const double v = b.measured((breaks.at(j) + breaks.at(j + 1)) / 2.0);
                    steps << at(breaks.at(j), v) << at(breaks.at(j + 1), v);
                }
                painter->drawPolyline(steps);
            }
            // The model: PAM4's eyes solid, dashed, dotted.
            static const Qt::PenStyle styles[] = {Qt::SolidLine, Qt::DashLine, Qt::DotLine};
            painter->setPen(QPen(ink, std::max(1, g->Thick), styles[k % 3], Qt::RoundCap, Qt::RoundJoin));
            QPolygonF line;
            for (int j = 0; j <= 800; ++j) {
                const double phase = xAxis.low + (xAxis.up - xAxis.low) * j / 800.0;
                line << at(phase, b.ber(phase));
            }
            painter->drawPolyline(line);
            double from = 0, to = 0;
            const double width = b.opening(ber, &from, &to) ? to - from : 0.0;
            if (narrowest < 0 || width < narrowestWidth) {
                narrowest = k;
                narrowestWidth = width;
            }
        }
        // The narrowest eye's opening at the target: its ends ticked, its
        // width written under the line.
        if (narrowest < 0) continue;
        const eye::Bathtub& b = tubs.at(narrowest);
        double from = 0, to = 0;
        if (!b.opening(ber, &from, &to)) continue;
        const QPointF a = at(from, ber), z = at(to, ber);
        painter->setPen(QPen(ink, 1.5));
        painter->drawLine(a, z);
        painter->drawLine(a + QPointF(0, -5), a + QPointF(0, 5));
        painter->drawLine(z + QPointF(0, -5), z + QPointF(0, 5));
        const QString text = QObject::tr("%1 UI (%2)").arg(QString::number(to - from, 'f', 3), EyeDiagram::engineering((to - from) * b.ui, QStringLiteral("s")));
        painter->save();
        painter->translate((a + z) / 2.0);
        painter->scale(1, -1);
        const QFontMetricsF fm(painter->font());
        painter->drawText(QPointF(-fm.horizontalAdvance(text) / 2.0, fm.ascent() + 4 + i * fm.height()), text);
        painter->restore();
    }
    painter->restore();
}

void BathtubDiagram::paintVoltage(QPainter* painter)
{
    // (In its coordinates: origin at the lower left corner, y up.)
    painter->save();
    painter->setClipRect(QRectF(0, 0, x2, y2));
    painter->setRenderHint(QPainter::Antialiasing, true);
    const double bottom = floorRate() / 10.0;   // below the axis: drawn out of the frame
    const auto at = [&](double threshold, double rate) {
        const double y[2] = {std::max(rate, bottom), 0};
        float px = 0, py = 0;
        calcCoordinate(&threshold, y, nullptr, &px, &py, &yAxis);
        return QPointF(px, py);
    };
    painter->setPen(QPen(QColor(128, 128, 128), 1, Qt::DashLine));
    painter->drawLine(at(xAxis.low, ber), at(xAxis.up, ber));
    for (int i = 0; i < m_volts.size() && i < Graphs.size(); ++i) {
        const Graph* g = Graphs.at(i);
        const QColor ink = qucs_s::ink::on(g->Color);
        const QString unit = qucs_s::dataset::unitOf(g->Var.section(QLatin1Char('/'), -1));
        const QList<eye::VoltageBathtub>& tubs = m_volts.at(i);
        for (int k = 0; k < tubs.size(); ++k) {
            const eye::VoltageBathtub& b = tubs.at(k);
            if (!b.ok()) continue;
            // The symbols counted: steps, thin.
            if (measured) {
                QVector<double> breaks = b.low.values + b.high.values;
                breaks << xAxis.low << xAxis.up;
                std::sort(breaks.begin(), breaks.end());
                QColor light = ink;
                light.setAlpha(150);
                painter->setPen(QPen(light, 1));
                QPolygonF steps;
                for (int j = 0; j + 1 < breaks.size(); ++j) {
                    if (breaks.at(j + 1) <= breaks.at(j)) continue;
                    const double v = b.measured((breaks.at(j) + breaks.at(j + 1)) / 2.0);
                    steps << at(breaks.at(j), v) << at(breaks.at(j + 1), v);
                }
                painter->drawPolyline(steps);
            }
            // The model: PAM4's eyes solid, dashed, dotted.
            static const Qt::PenStyle styles[] = {Qt::SolidLine, Qt::DashLine, Qt::DotLine};
            painter->setPen(QPen(ink, std::max(1, g->Thick), styles[k % 3], Qt::RoundCap, Qt::RoundJoin));
            QPolygonF line;
            for (int j = 0; j <= 800; ++j) {
                const double v = xAxis.low + (xAxis.up - xAxis.low) * j / 800.0;
                line << at(v, b.ber(v));
            }
            painter->drawPolyline(line);
            // Its opening at the target: the ends ticked, the height under it.
            double from = 0, to = 0;
            if (!b.opening(ber, &from, &to)) continue;
            const QPointF a = at(from, ber), z = at(to, ber);
            painter->setPen(QPen(ink, 1.5));
            painter->drawLine(a, z);
            painter->drawLine(a + QPointF(0, -5), a + QPointF(0, 5));
            painter->drawLine(z + QPointF(0, -5), z + QPointF(0, 5));
            const QString text = EyeDiagram::engineering(to - from, unit);
            painter->save();
            painter->translate((a + z) / 2.0);
            painter->scale(1, -1);
            const QFontMetricsF fm(painter->font());
            painter->drawText(QPointF(-fm.horizontalAdvance(text) / 2.0, fm.ascent() + 4 + i * fm.height()), text);
            painter->restore();
        }
    }
    painter->restore();
}

QStringList BathtubDiagram::voltageSummary() const
{
    QStringList lines;
    for (int i = 0; i < m_volts.size() && i < Graphs.size(); ++i) {
        const QString name = Graphs.at(i)->Var.section(QLatin1Char('/'), -1);
        const QString unit = qucs_s::dataset::unitOf(name);
        const QList<eye::VoltageBathtub>& tubs = m_volts.at(i);
        if (tubs.isEmpty()) continue;
        if (tubs.size() == 1 && tubs.first().symbols == 0 && !tubs.first().ok()) {
            lines << QObject::tr("%1: %2").arg(name, tubs.first().error);
            continue;
        }
        lines << QObject::tr("%1: %2 symbols").arg(name).arg(tubs.first().symbols);
        for (int k = 0; k < tubs.size(); ++k) {
            const eye::VoltageBathtub& b = tubs.at(k);
            const QString eyeName = tubs.size() > 1 ? QObject::tr("eye %1: ").arg(k + 1) : QString();
            if (!b.ok()) {
                lines << eyeName + b.error;
                continue;
            }
            // (Short lines: the box is as wide as its longest.)
            const QString sigma = QString(QChar(0x03C3)), indent = tubs.size() > 1 ? QStringLiteral("   ") : QString();
            const QString q = std::isfinite(b.q()) ? QObject::tr("Q %1, BER %2").arg(QString::number(b.q(), 'f', 2), rateText(b.berOfQ()))
                                                   : QObject::tr("Q infinite (no noise)");
            double from = 0, to = 0;
            const QString open = b.opening(ber, &from, &to) ? EyeDiagram::engineering(to - from, unit) : QObject::tr("closed");
            lines << eyeName
                         + QObject::tr("levels %1 %2 %3, %4 %5 %6")
                               .arg(EyeDiagram::engineering(b.low.mean, unit), sigma, EyeDiagram::engineering(b.low.sigma, unit),
                                    EyeDiagram::engineering(b.high.mean, unit), sigma, EyeDiagram::engineering(b.high.sigma, unit));
            lines << indent + QObject::tr("%1; best %2").arg(q, EyeDiagram::engineering(b.best(), unit));
            lines << indent + QObject::tr("opening %1 at %2").arg(open, rateText(ber));
        }
    }
    return lines;
}

QStringList BathtubDiagram::summary() const
{
    QStringList lines;
    for (int i = 0; i < m_tubs.size() && i < Graphs.size(); ++i) {
        const QString name = Graphs.at(i)->Var.section(QLatin1Char('/'), -1);
        const QList<eye::Bathtub>& tubs = m_tubs.at(i);
        if (tubs.isEmpty()) continue;
        if (tubs.size() == 1 && !tubs.first().ok()) {
            lines << QObject::tr("%1: %2").arg(name, tubs.first().error);
            continue;
        }
        const eye::Bathtub& first = tubs.first();
        lines << QObject::tr("%1: UI %2, transitions %3 %").arg(name, EyeDiagram::engineering(first.ui, QStringLiteral("s")),
                                                                 QString::number(100 * first.density, 'f', 0));
        for (int k = 0; k < tubs.size(); ++k) {
            const eye::Bathtub& b = tubs.at(k);
            const QString eyeName = tubs.size() > 1 ? QObject::tr("eye %1: ").arg(k + 1) : QString();
            if (!b.ok()) {
                lines << eyeName + b.error;
                continue;
            }
            double from = 0, to = 0;
            const double open = b.opening(ber, &from, &to) ? to - from : 0.0;
            lines << eyeName
                         + QObject::tr("RJ %1 rms, DJ %2%3; TJ %4 at %5")
                               .arg(EyeDiagram::engineering(b.rj() * b.ui, QStringLiteral("s")),
                                    EyeDiagram::engineering(b.dj() * b.ui, QStringLiteral("s")),
                                    b.dualDirac ? QString() : QObject::tr(" (one Gaussian)"),
                                    EyeDiagram::engineering((1.0 - open) * b.ui, QStringLiteral("s")), rateText(ber));
        }
    }
    return lines;
}

void BathtubDiagram::paintInFront(QPainter* painter, const Colors& colors)
{
    RectDiagram::paintInFront(painter, colors);
    const QStringList lines = direction == Voltage ? voltageSummary() : summary();
    if (lines.isEmpty()) return;
    // (y down.) At the top in the middle, between the walls.
    painter->save();
    const QFontMetricsF fm(painter->font());
    double width = 0;
    for (const QString& l : lines) width = std::max(width, fm.horizontalAdvance(l));
    const QRectF box((x2 - width) / 2.0 - 4, -y2 + 4, width + 8, lines.size() * fm.height() + 6);
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

QString BathtubDiagram::extraSaveFields() const
{
    return QStringLiteral(" %1 %2 %3 %4 %5 %6 %7 %8 %9")
        .arg(fieldText(ui), fieldText(start))
        .arg(levels)
        .arg(fieldText(threshold), fieldText(ber), fieldText(floor))
        .arg(measured ? 1 : 0)
        .arg(direction)
        .arg(fieldText(phase));
}

void BathtubDiagram::loadExtraFields(const QStringList& fields)
{
    bool ok = false;
    ui = fieldValue(fields.value(0));
    if (!(ui > 0.0)) ui = eye::NaN;
    start = fieldValue(fields.value(1));
    const int l = fields.value(2).toInt(&ok);
    levels = ok && (l == 2 || l == 4) ? l : 0;
    threshold = fieldValue(fields.value(3));
    const double rate = fieldValue(fields.value(4));
    ber = rate > 0.0 && rate <= 0.01 ? rate : 1e-12;
    floor = fieldValue(fields.value(5));
    if (!(floor > 0.0 && floor < ber)) floor = eye::NaN;
    measured = fields.value(6) != QLatin1String("0");
    // (A file made before they were has neither: across, at the centre.)
    direction = fields.value(7) == QLatin1String("1") ? Voltage : Timing;
    const double p = fieldValue(fields.value(8));
    phase = std::isfinite(p) ? std::clamp(p, -0.5, 0.5) : 0.0;
}
