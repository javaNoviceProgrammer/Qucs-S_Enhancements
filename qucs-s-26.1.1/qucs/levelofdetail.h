/*
 * levelofdetail.h - what the canvas leaves out when zoomed out too far to
 *                   show it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_LEVELOFDETAIL_H
#define QUCS_LEVELOFDETAIL_H

/*!
 * \brief Texts too small to read, left out of the canvas.
 *
 * Zoomed out over a large schematic, the names and values of thousands of
 * components are a pixel or two tall - nothing anyone can read - and yet
 * laying them out took half of every repaint. The canvas leaves them out
 * then: component names and properties, the texts of symbols, the names
 * of pins. Prints and exports draw them whatever their size.
 *
 * The choice is made for the time of a paint with a HiddenTexts object;
 * nothing set means texts are drawn.
 */
namespace qucs_s::lod {

/// The height, in pixels on the screen, of a line of text below which the
/// canvas leaves texts out: a capital letter about two pixels tall.
constexpr double kSmallestLine = 4.0;

/// Texts left out while this lives, if \a hidden (they nest).
class HiddenTexts
{
public:
    explicit HiddenTexts(bool hidden);
    ~HiddenTexts();
    HiddenTexts(const HiddenTexts&) = delete;
    HiddenTexts& operator=(const HiddenTexts&) = delete;
private:
    bool m_previous;
};

/// Whether texts are drawn (true when no HiddenTexts says otherwise).
bool textsShown();

/*!
 * \brief A zoom under way: a graph's lines drawn quickly.
 *
 * A diagram's graph is a line from each point to the next, each with round
 * caps; wider than a pixel, Qt outlines every cap as curves before it fills
 * it. A page of sweeps (sixty curves of 2,000 points) took 147 ms a paint,
 * and a zoom by wheel or pinch choked on it. While the canvas zooms, the
 * caps are square - their width and colour as they are, some six times
 * quicker -; the zoom done, round again.
 *
 * Set for the time of a paint with a Draft object; nothing set, the
 * lines are drawn in full. Prints and exports are.
 */
class Draft
{
public:
    explicit Draft(bool draft);
    ~Draft();
    Draft(const Draft&) = delete;
    Draft& operator=(const Draft&) = delete;
private:
    bool m_previous;
};

/// Whether a zoom's draft is drawn (false when no Draft says otherwise).
bool draft();

/// A graph's lines this wide, in pixels of the device, or less have square
/// caps whatever: a square end differs from a round one by under half a
/// pixel there - on a raster's pixels (VectorOutput).
constexpr double kSquareCapsWidth = 3.0;

/*!
 * \brief Output to be scaled after: drawn as exactly as asked.
 *
 * An SVG, PDF or EPS export, or a print, is drawn at its own scale and then
 * zoomed in a viewer or printed large: a graph's lines squared where a
 * pixel of that scale hides it, or thinned to a pixel of it, show. While a
 * VectorOutput lives a graph's lines keep their round caps and are thinned
 * to a tenth of a pixel. Set by the exports and printing.
 */
class VectorOutput
{
public:
    explicit VectorOutput(bool vector = true);
    ~VectorOutput();
    VectorOutput(const VectorOutput&) = delete;
    VectorOutput& operator=(const VectorOutput&) = delete;
private:
    bool m_previous;
};

/// Whether output is a vector's (false when no VectorOutput says so).
bool vectorOutput();

} // namespace qucs_s::lod

#endif
