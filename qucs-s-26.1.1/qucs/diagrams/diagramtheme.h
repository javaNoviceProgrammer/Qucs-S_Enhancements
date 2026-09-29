/*
 * diagramtheme.h - the colours of a diagram's parts: its background, the
 *                  area inside its frame, the frame, the grid, each axis,
 *                  its title, a table's texts, the legend (the diagram
 *                  dialog's Theme tab)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_DIAGRAMTHEME_H
#define QUCS_DIAGRAMTHEME_H

#include <QColor>
#include <QList>
#include <QPair>
#include <QPen>
#include <QString>

#include <array>
#include <optional>

/*!
 * \brief The colours a diagram is drawn in, part by part.
 *
 * Each part is automatic or in a colour chosen for it. Automatic is how
 * Qucs-S has drawn diagrams: no background on light paper, a white card
 * on dark paper, black frame, axes and texts, a light grey grid, a white
 * legend with a grey border - each fitted to the background it is drawn
 * on (ink::on()): black on a dark background chosen turns light. The
 * traces keep their own colours, and markers theirs (their automatic
 * ones are those of the paper under them, the plot area).
 */
namespace qucs_s::diagramtheme {

/// The parts of a diagram, each drawn in a colour of its own.
enum class Part : unsigned char {
    None = 0,          ///< not a part: drawn in its own colour (a trace, a warning)
    Background,        ///< under all of it: its frame, its numbers, labels and title
    PlotArea,          ///< inside its frame (the circle of a polar or Smith chart)
    Frame,             ///< the frame; a table's rules; a 3D diagram's box
    Grid,
    XAxis,             ///< the x axis' ticks, numbers and label (and its line, when it has one of its own)
    YAxis,             ///< the y axis' (the left one)
    RightAxis,         ///< the right y axis' (a 3D diagram's z axis; a polar-Smith chart's second one)
    Title,
    Text,              ///< a table's texts
    LegendBackground,
    LegendBorder,
    LegendText,
};
constexpr int kParts = int(Part::LegendText) + 1;

/// All parts, in the order the Theme tab lists them.
QList<Part> allParts();
/// The name a part has in a saved theme and in Claude's tools
/// ("background", "plot_area", "x_axis", "legend_text", ...).
QString keyOf(Part part);
std::optional<Part> partNamed(const QString& key);

/// A colour written as a saved theme writes it: #rrggbb, or #aarrggbb
/// when it is see-through.
QString colorText(const QColor& color);

/// The colours chosen for a diagram's parts; the others are automatic.
class Theme
{
public:
    /// The colour chosen for \a part; invalid when it is automatic.
    QColor chosen(Part part) const;
    /// Chooses \a color for \a part; an invalid one makes it automatic.
    void choose(Part part, const QColor& color);
    bool isAutomatic() const;
    /// The colours chosen, as a diagram's line keeps them:
    /// "background=#1e1f22 frame=#c8c8c8"; empty when all are automatic.
    QString toString() const;
    /// Read back; a name not known or a colour that does not read is
    /// left out (automatic).
    static Theme fromString(const QString& text);

    bool operator==(const Theme& other) const;
    bool operator!=(const Theme& other) const { return !(*this == other); }

private:
    std::array<QColor, kParts> m_colors;
};

/// A theme's colours as they are drawn on \a paper (the canvas's colour,
/// white on prints and exports): every part in a colour, the automatic
/// ones fitted to what is under them.
struct Colors {
    QColor card;      ///< what the background is filled with; invalid: nothing
    QColor plotArea;  ///< what the plot area is filled with; invalid: nothing
    QColor outside;   ///< the paper under the frame, numbers, labels and title
    QColor inside;    ///< the paper inside the frame, under the traces and markers
    QColor legend;    ///< the paper inside the legend
    std::array<QColor, kParts> part;

    QColor of(Part p) const { return part[size_t(p)]; }
    /// \a pen in the colour of \a p (a primitive's part); as it is for None.
    QPen pen(QPen pen, unsigned char p) const
    {
        if (p != 0 && p < kParts) pen.setColor(part[p]);
        return pen;
    }
};
/// \a numbersInside: the numbers of the y axes are inside the frame (a
/// polar or Smith chart's), and their automatic colour fits the plot area.
Colors colorsOn(const Theme& theme, const QColor& paper, bool numbersInside = false);

/// The grid's colour when it is automatic, on light paper (Qt::lightGray,
/// what diagrams have always been saved with).
QColor defaultGridColor();

/// Ready-made themes the Theme tab starts from.
enum class Preset { Automatic, Light, Dark, NoCard };
QList<QPair<Preset, QString>> presets();
Theme preset(Preset preset);

/// The theme a diagram placed from now on starts with (the Theme tab's
/// "Save as Default for New Diagrams"); all automatic unless one was saved.
Theme defaultForNewDiagrams();
void setDefaultForNewDiagrams(const Theme& theme);

} // namespace qucs_s::diagramtheme

#endif
