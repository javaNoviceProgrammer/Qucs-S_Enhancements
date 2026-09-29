/*
 * diagramtheme.cpp - the colours of a diagram's parts
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "diagramtheme.h"

#include "ink.h"
#include "settings.h"

#include <QCoreApplication>
#include <QStringList>

namespace qucs_s::diagramtheme {

namespace {

const char* const kKeys[kParts] = {"",      "background", "plot_area", "frame", "grid",
                                   "x_axis", "y_axis",    "y2_axis",   "title", "text",
                                   "legend_background", "legend_border", "legend_text"};

const char* const kDefaultKey = "DiagramTheme";

QString tr(const char* text) { return QCoreApplication::translate("DiagramTheme", text); }

// \a top laid over \a under (paper, opaque).
QColor over(const QColor& top, const QColor& under)
{
    if (!top.isValid()) return under;
    const double a = top.alphaF();
    return QColor::fromRgbF(float(top.redF() * a + under.redF() * (1 - a)), float(top.greenF() * a + under.greenF() * (1 - a)),
                            float(top.blueF() * a + under.blueF() * (1 - a)));
}

// \a a moved \a share of the way to \a b.
QColor mix(const QColor& a, const QColor& b, double share)
{
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * share), float(a.greenF() + (b.greenF() - a.greenF()) * share),
                            float(a.blueF() + (b.blueF() - a.blueF()) * share));
}

// \a colour as it shows on \a paper (ink::on()).
QColor fitted(const QColor& colour, const QColor& paper)
{
    const ink::Paper on(paper);
    return ink::on(colour);
}

} // namespace

QList<Part> allParts()
{
    QList<Part> all;
    for (int p = 1; p < kParts; ++p) all << Part(p);
    return all;
}

QString keyOf(Part part)
{
    return QString::fromLatin1(kKeys[size_t(part)]);
}

std::optional<Part> partNamed(const QString& key)
{
    for (int p = 1; p < kParts; ++p)
        if (key == QLatin1String(kKeys[p])) return Part(p);
    return std::nullopt;
}

QString colorText(const QColor& color)
{
    return color.name(color.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb);
}

QColor Theme::chosen(Part part) const
{
    return m_colors[size_t(part)];
}

void Theme::choose(Part part, const QColor& color)
{
    if (part == Part::None) return;
    m_colors[size_t(part)] = color.isValid() ? color : QColor();
}

bool Theme::isAutomatic() const
{
    for (const QColor& c : m_colors)
        if (c.isValid()) return false;
    return true;
}

QString Theme::toString() const
{
    QStringList parts;
    for (int p = 1; p < kParts; ++p)
        if (m_colors[p].isValid()) parts << keyOf(Part(p)) + QLatin1Char('=') + colorText(m_colors[p]);
    return parts.join(QLatin1Char(' '));
}

Theme Theme::fromString(const QString& text)
{
    Theme theme;
    for (const QString& item : text.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const std::optional<Part> part = partNamed(item.section(QLatin1Char('='), 0, 0));
        const QColor color = QColor::fromString(item.section(QLatin1Char('='), 1));
        if (part && color.isValid()) theme.choose(*part, color);
    }
    return theme;
}

bool Theme::operator==(const Theme& other) const
{
    for (int p = 0; p < kParts; ++p)
        if (m_colors[p].isValid() != other.m_colors[p].isValid()
            || (m_colors[p].isValid() && m_colors[p].rgba() != other.m_colors[p].rgba()))
            return false;
    return true;
}

QColor defaultGridColor()
{
    return QColor(Qt::lightGray);
}

Colors colorsOn(const Theme& theme, const QColor& paper, bool numbersInside)
{
    Colors c;
    const QColor canvas = paper.isValid() ? paper.toRgb() : QColor(Qt::white);
    // The background: the one chosen (see-through, none), or on dark paper
    // the light card diagrams are drawn on.
    c.card = theme.chosen(Part::Background);
    if (!c.card.isValid() && ink::isDark(canvas)) c.card = QColor(Qt::white);
    c.outside = over(c.card, canvas);
    c.plotArea = theme.chosen(Part::PlotArea);
    c.inside = over(c.plotArea, c.outside);
    c.part[size_t(Part::Background)] = c.card;
    c.part[size_t(Part::PlotArea)] = c.plotArea;

    // Each part in the colour chosen, or the one it has always had fitted
    // to what it is drawn on.
    const auto set = [&](Part p, const QColor& automatic) {
        const QColor chosen = theme.chosen(p);
        c.part[size_t(p)] = chosen.isValid() ? chosen : automatic;
    };
    set(Part::Frame, fitted(Qt::black, c.outside));
    set(Part::XAxis, fitted(Qt::black, c.outside));
    const QColor yPaper = numbersInside ? c.inside : c.outside;
    set(Part::YAxis, fitted(Qt::black, yPaper));
    set(Part::RightAxis, fitted(Qt::black, yPaper));
    set(Part::Title, fitted(Qt::black, c.outside));
    set(Part::Text, fitted(Qt::black, c.inside));
    // A faint grid: light grey on light paper, a little lighter than dark
    // paper on dark paper.
    set(Part::Grid, ink::isDark(c.inside) ? mix(c.inside, Qt::white, 0.22) : defaultGridColor());
    // The legend: nearly opaque white on light paper, a shade of dark
    // paper on dark.
    QColor legend = ink::isDark(c.inside) ? mix(c.inside, Qt::white, 0.06) : QColor(Qt::white);
    legend.setAlpha(230);
    set(Part::LegendBackground, legend);
    c.legend = over(c.of(Part::LegendBackground), c.inside);
    set(Part::LegendBorder, fitted(Qt::darkGray, c.inside));
    set(Part::LegendText, fitted(Qt::black, c.legend));
    return c;
}

QList<QPair<Preset, QString>> presets()
{
    return {{Preset::Automatic, tr("Automatic")},
            {Preset::Light, tr("Light")},
            {Preset::Dark, tr("Dark")},
            {Preset::NoCard, tr("No background (on the canvas)")}};
}

Theme preset(Preset preset)
{
    Theme t;
    switch (preset) {
    case Preset::Automatic:
        break;
    case Preset::Light:
        // White, whatever the canvas: the rest as it has always been.
        t.choose(Part::Background, Qt::white);
        break;
    case Preset::Dark:
        t.choose(Part::Background, QColor(0x1e, 0x1f, 0x22));
        t.choose(Part::PlotArea, QColor(0x26, 0x28, 0x2c));
        t.choose(Part::Frame, QColor(0x8c, 0x8f, 0x94));
        t.choose(Part::Grid, QColor(0x3c, 0x3f, 0x44));
        t.choose(Part::XAxis, QColor(0xc8, 0xca, 0xcd));
        t.choose(Part::YAxis, QColor(0xc8, 0xca, 0xcd));
        t.choose(Part::RightAxis, QColor(0xc8, 0xca, 0xcd));
        t.choose(Part::Title, QColor(0xe6, 0xe7, 0xe9));
        t.choose(Part::Text, QColor(0xd4, 0xd6, 0xd9));
        t.choose(Part::LegendBackground, QColor(0x2b, 0x2d, 0x30, 230));
        t.choose(Part::LegendBorder, QColor(0x6b, 0x6e, 0x73));
        t.choose(Part::LegendText, QColor(0xdf, 0xe1, 0xe4));
        break;
    case Preset::NoCard:
        // Nothing under it: the canvas shows through, and the automatic
        // colours fit the canvas.
        t.choose(Part::Background, Qt::transparent);
        break;
    }
    return t;
}

Theme defaultForNewDiagrams()
{
    return Theme::fromString(_settings::Get().item<QString>(QLatin1String(kDefaultKey)));
}

void setDefaultForNewDiagrams(const Theme& theme)
{
    _settings::Get().setItem<QString>(QLatin1String(kDefaultKey), theme.toString());
}

} // namespace qucs_s::diagramtheme
