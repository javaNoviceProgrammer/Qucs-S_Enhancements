/*
 * spectrumdiagram.cpp - the spectrum view: a transient's spectrum in dBc,
 * its harmonics numbered, THD, SFDR, SNR and SINAD written beside it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "spectrumdiagram.h"

#include "ink.h"
#include "main.h"
#include "misc.h"

#include <QPainter>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace sp = qucs_s::spectrum;

namespace {

QString fieldText(double v)
{
    return std::isfinite(v) ? QString::number(v, 'g', 15) : QStringLiteral("-");
}

// \a v with four significant digits and an SI prefix: "1 kHz".
QString engineering(double v, const QString& unit)
{
    static const char* const prefixes[] = {"f", "p", "n", "\u00B5", "m", "", "k", "M", "G", "T"};
    if (v == 0.0 || !std::isfinite(v)) return QString::number(v) + QLatin1Char(' ') + unit;
    const int e = std::clamp(int(std::floor(std::log10(std::fabs(v)) / 3.0)), -5, 4);
    return QString::number(v / std::pow(1000.0, e), 'g', 4) + QLatin1Char(' ') + QString::fromUtf8(prefixes[e + 5]) + unit;
}

double fieldValue(const QString& s)
{
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok && std::isfinite(v) ? v : std::nan("");
}

} // namespace

SpectrumDiagram::SpectrumDiagram(int cx, int cy) : RectDiagram(cx, cy), from(std::nan("")), fundamental(std::nan(""))
{
    x2 = 400;
    y2 = 260;
    x3 = x2 + 7;
    Name = "Spectrum";
    calcDiagram();
}

Diagram* SpectrumDiagram::newOne()
{
    return new SpectrumDiagram();
}

Element* SpectrumDiagram::info(QString& Name, char*& BitmapFile, bool getNewOne)
{
    Name = QObject::tr("Spectrum");
    BitmapFile = (char*)"spectrum";
    if (getNewOne) return new SpectrumDiagram();
    return nullptr;
}

SpectrumDiagram::Result SpectrumDiagram::compute(const Graph* g) const
{
    Result r;
    const DataX* xs = g->axis(0);
    if (xs == nullptr || xs->Points == nullptr || g->cPointsY == nullptr) return r;
    QVector<double> x(xs->count), y(xs->count);
    for (int i = 0; i < xs->count; ++i) {
        x[i] = xs->Points[i];
        y[i] = g->cPointsY[2 * i];
    }
    r.spectrum = sp::of(x, y, window, from);
    r.analysis = sp::analyse(r.spectrum, fundamental, harmonics);
    return r;
}

double SpectrumDiagram::shown(double amplitude, const Result& r) const
{
    const double reference = dbc && r.analysis.ok ? r.analysis.amplitude : 1.0;
    return amplitude > 0 && reference > 0 ? 20.0 * std::log10(amplitude / reference) : -400.0;
}

void SpectrumDiagram::getAxisLimits(Graph* g)
{
    const Result r = compute(g);
    m_results.insert(g, r);
    Axis* pa = graphAxis(g);
    ++pa->numGraphs;
    const sp::Spectrum& s = r.spectrum;
    if (s.amplitude.isEmpty()) return;
    // Along x: to a little past the highest harmonic numbered (else all
    // of it); up: from below the lowest line or the noise floor to the top.
    const double nyquist = s.df * s.amplitude.size();
    const double top = r.analysis.ok ? std::min(nyquist, (harmonics + 1.0) * r.analysis.fundamental) : nyquist;
    xAxis.min = std::min(xAxis.min, 0.0);
    xAxis.max = std::max(xAxis.max, top);
    double low = 0, high = -DBL_MAX;
    for (int k = 1; k < s.amplitude.size() && k * s.df <= top; ++k) high = std::max(high, shown(s.amplitude.at(k), r));
    if (r.analysis.ok) {
        low = std::min(low, r.analysis.noiseFloorDbc + (dbc ? 0.0 : shown(r.analysis.amplitude, r)));
        for (const sp::Line& l : r.analysis.harmonics) low = std::min(low, shown(l.amplitude, r));
    }
    low = std::max(low - 10.0, (std::isfinite(high) ? high : 0.0) - 200.0);
    pa->min = std::min(pa->min, low);
    pa->max = std::max(pa->max, high + 5.0);
}

int SpectrumDiagram::calcDiagram()
{
    yAxis.log = false;
    return RectDiagram::calcDiagram();
}

void SpectrumDiagram::createAxisLabels()
{
    const QString x = xAxis.Label, y = yAxis.Label;
    if (x.isEmpty()) xAxis.Label = QObject::tr("frequency (Hz)");
    if (y.isEmpty()) yAxis.Label = dbc ? QObject::tr("dBc") : QObject::tr("dB");
    RectDiagram::createAxisLabels();
    xAxis.Label = x;
    yAxis.Label = y;
}

void SpectrumDiagram::paintBehindGraphs(QPainter* painter)
{
    // (In its coordinates: origin at the lower left corner, y up.)
    painter->save();
    painter->setClipRect(QRectF(0, 0, x2, y2));
    painter->setRenderHint(QPainter::Antialiasing, true);
    const auto at = [&](double f, double value, const Axis* a) {
        const double y[2] = {value, 0};
        float px = 0, py = 0;
        calcCoordinate(&f, y, nullptr, &px, &py, a);
        return QPointF(px, py);
    };
    for (const Graph* g : Graphs) {
        const Result r = m_results.value(g);
        const sp::Spectrum& s = r.spectrum;
        if (s.amplitude.isEmpty()) continue;
        const Axis* a = graphAxis(g);
        const QColor ink = qucs_s::ink::on(g->Color);
        painter->setPen(QPen(ink, std::max(1, g->Thick), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPolygonF line;
        for (int k = xAxis.log ? 1 : 0; k < s.amplitude.size(); ++k) {
            const QPointF p = at(k * s.df, shown(s.amplitude.at(k), r), a);
            if (stems) painter->drawLine(QPointF(p.x(), 0), p);
            else line << p;
        }
        if (!stems) painter->drawPolyline(line);
        // Its fundamental and harmonics: a circle each, numbered.
        if (!r.analysis.ok) continue;
        for (const sp::Line& l : r.analysis.harmonics) {
            const QPointF p = at(l.frequency, shown(l.amplitude, r), a);
            painter->setPen(QPen(ink, 1.5));
            painter->setBrush(Qt::NoBrush);
            painter->drawEllipse(p, 4.0, 4.0);
            painter->save();
            painter->translate(p);
            painter->scale(1, -1);
            painter->drawText(QPointF(-3, -7), l.harmonic == 1 ? QStringLiteral("f0") : QString::number(l.harmonic));
            painter->restore();
        }
    }
    painter->restore();
}

QStringList SpectrumDiagram::summary() const
{
    QStringList lines;
    for (const Graph* g : Graphs) {
        const Result r = m_results.value(g);
        if (!r.analysis.ok) {
            if (!r.analysis.error.isEmpty()) lines << QObject::tr("%1: %2").arg(g->Var.section(QLatin1Char('/'), -1), r.analysis.error);
            continue;
        }
        const sp::Analysis& a = r.analysis;
        lines << QObject::tr("%1: f0 %2, %3 window")
                     .arg(g->Var.section(QLatin1Char('/'), -1), engineering(a.fundamental, QStringLiteral("Hz")),
                          sp::windowName(window).replace(QLatin1Char('_'), QLatin1Char(' ')));
        lines << QObject::tr("THD %1 % (%2 dB)  SFDR %3 dB")
                     .arg(QString::number(100 * a.thd, 'g', 3), QString::number(a.thd > 0 ? 20 * std::log10(a.thd) : -400, 'f', 1),
                          QString::number(a.sfdr, 'f', 1));
        lines << QObject::tr("SNR %1 dB  SINAD %2 dB").arg(QString::number(a.snr, 'f', 1), QString::number(a.sinad, 'f', 1));
    }
    return lines;
}

void SpectrumDiagram::paintInFront(QPainter* painter, const Colors& colors)
{
    RectDiagram::paintInFront(painter, colors);
    const QStringList lines = summary();
    if (lines.isEmpty()) return;
    // (y down.) In the upper right corner, on the legend's background.
    painter->save();
    const QFontMetricsF fm(painter->font());
    double width = 0;
    for (const QString& l : lines) width = std::max(width, fm.horizontalAdvance(l));
    const QRectF box(x2 - width - 12, -y2 + 4, width + 8, lines.size() * fm.height() + 6);
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

QString SpectrumDiagram::extraSaveFields() const
{
    return QStringLiteral(" %1 %2 %3 %4 %5 %6")
        .arg(int(window))
        .arg(harmonics)
        .arg(dbc ? 1 : 0)
        .arg(stems ? 1 : 0)
        .arg(fieldText(from), fieldText(fundamental));
}

void SpectrumDiagram::loadExtraFields(const QStringList& fields)
{
    bool ok = false;
    const int w = fields.value(0).toInt(&ok);
    window = ok && w >= 0 && w <= int(sp::Window::FlatTop) ? sp::Window(w) : sp::Window::Hann;
    const int h = fields.value(1).toInt(&ok);
    harmonics = ok && h >= 2 && h <= 50 ? h : 9;
    dbc = fields.value(2) != QLatin1String("0");
    stems = fields.value(3) == QLatin1String("1");
    from = fieldValue(fields.value(4));
    fundamental = fieldValue(fields.value(5));
    if (!(fundamental > 0)) fundamental = std::nan("");
}
