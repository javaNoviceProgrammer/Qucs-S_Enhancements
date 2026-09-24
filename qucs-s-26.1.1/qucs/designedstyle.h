/*
 * designedstyle.h - the style the designed themes draw with: Fusion, its
 * controls rounded and flat
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_DESIGNEDSTYLE_H
#define QUCS_DESIGNEDSTYLE_H

#include <QProxyStyle>

namespace qucs_s::apptheme {

/*!
 * \brief Fusion with the controls of the designed themes.
 *
 * Push buttons, fields, check boxes and radio buttons are drawn flat and
 * rounded, in the palette's colours only - the button colour filled, the
 * Mid colour for the frame, the highlight for focus, a checked box and a
 * default button - so a button whose palette carries a colour (a colour
 * picker) shows it. On a dark palette the icons of tool bars are inked
 * (ink::inked()): a symbol drawn in dark blue for light paper shows. The
 * rest is Fusion's, and the theme's style sheet.
 */
class DesignedStyle : public QProxyStyle
{
public:
    DesignedStyle();

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget = nullptr) const override;
    int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
                    const QWidget* widget = nullptr) const override;
};

} // namespace qucs_s::apptheme

#endif
