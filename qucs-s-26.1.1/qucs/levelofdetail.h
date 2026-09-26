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

} // namespace qucs_s::lod

#endif
