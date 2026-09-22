/*
 * ink.cpp - the colours a schematic is drawn in, fitted to its paper
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "ink.h"

#include <QHash>

#include <cmath>

namespace qucs_s::ink {

namespace {

// Contrast a line or a text needs to be read without effort (WCAG's
// figure for large text and graphics).
constexpr double kEnough = 3.0;
// A paper this dark or darker counts as dark.
constexpr double kDarkPaper = 0.18;

QColor& current()
{
    static QColor paper(Qt::white);
    return paper;
}

// What on() made of each colour on each paper; a paint asks the same few
// colours thousands of times.
QHash<quint64, QRgb>& cache()
{
    static QHash<quint64, QRgb> c;
    return c;
}

double channel(double c)
{
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QColor& c)
{
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}

} // namespace

Paper::Paper(const QColor& paper) : m_previous(current())
{
    current() = paper.isValid() ? paper : QColor(Qt::white);
}

Paper::~Paper() { current() = m_previous; }

QColor paper() { return current(); }

bool isDark(const QColor& paper) { return paper.isValid() && luminance(paper) < kDarkPaper; }

bool darkPaper() { return isDark(current()); }

double contrast(const QColor& a, const QColor& b)
{
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QColor on(const QColor& colour)
{
    if (!colour.isValid() || colour.alpha() == 0 || !darkPaper()) return colour;
    const QColor paper = current();
    const quint64 key = (quint64(paper.rgb()) << 32) | colour.rgba();
    auto hit = cache().constFind(key);
    if (hit != cache().constEnd()) return QColor::fromRgba(*hit);

    QColor result = colour;
    if (contrast(colour, paper) < kEnough) {
        // The lightness turned over (dark blue to light blue), then raised
        // until it shows; the hue and the saturation stay.
        const QColor hsl = colour.toHsl();
        const float hue = std::max(hsl.hslHueF(), 0.0f), saturation = hsl.hslSaturationF();
        float lightness = std::max(1.0f - hsl.lightnessF(), 0.55f);
        result = QColor::fromHslF(hue, saturation, std::min(lightness, 0.92f), colour.alphaF());
        while (lightness < 0.92f && contrast(result, paper) < kEnough) {
            lightness += 0.02f;
            result = QColor::fromHslF(hue, saturation, std::min(lightness, 0.92f), colour.alphaF());
        }
    }
    result = result.toRgb();   // one spec, whether from the cache or not
    if (cache().size() > 4096) cache().clear();
    cache().insert(key, result.rgba());
    return result;
}

QPen on(QPen pen)
{
    if (pen.style() != Qt::NoPen && pen.brush().style() == Qt::SolidPattern) pen.setColor(on(pen.color()));
    return pen;
}

QBrush on(QBrush brush)
{
    if (brush.style() != Qt::NoBrush && brush.gradient() == nullptr && brush.textureImage().isNull())
        brush.setColor(on(brush.color()));
    return brush;
}

} // namespace qucs_s::ink
