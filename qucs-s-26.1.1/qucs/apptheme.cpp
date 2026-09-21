/*
 * apptheme.cpp - the application's look: the system's, dark or light
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "apptheme.h"

#include <QApplication>
#include <QStyleHints>

namespace qucs_s::apptheme {

namespace {

// Whether a palette of ours is on the application, as opposed to the
// platform's: what "System" has to undo.
bool g_paletteForced = false;

// Whether the platform gives the requested appearance itself. Cocoa and
// Windows do (Qt 6.8 on); the generic Unix and offscreen platforms leave
// the request unanswered.
bool platformShows(Qt::ColorScheme scheme)
{
    return QGuiApplication::styleHints()->colorScheme() == scheme;
}

void forcePalette(const QPalette& palette)
{
    QApplication::setPalette(palette);
    g_paletteForced = true;
}

// Back to the platform's palette: a palette that resolves nothing of its
// own makes Qt take every role from the platform theme again, now and
// when the system's appearance changes later.
void releasePalette()
{
    if (!g_paletteForced) return;
    QPalette platform;
    platform.setResolveMask(0);
    QApplication::setPalette(platform);
    g_paletteForced = false;
}

} // namespace

int bounded(int theme)
{
    return theme == Dark || theme == Light ? theme : System;
}

void apply(int theme)
{
    theme = bounded(theme);
    QStyleHints* hints = QGuiApplication::styleHints();
    if (theme == System) {
        hints->unsetColorScheme();
        releasePalette();
        return;
    }
    const Qt::ColorScheme scheme = theme == Dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light;
    hints->setColorScheme(scheme);
    if (platformShows(scheme)) {
        releasePalette();
        return;
    }
    forcePalette(theme == Dark ? darkPalette() : lightPalette());
}

bool isDark()
{
    const QPalette p = QApplication::palette();
    return p.color(QPalette::WindowText).value() > p.color(QPalette::Window).value();
}

QPalette darkPalette()
{
    // The colours of the Fusion style's dark scheme, spelled out so that
    // every style shows them.
    const QColor window(0x35, 0x35, 0x35), base(0x23, 0x23, 0x23), text(0xe6, 0xe6, 0xe6);
    const QColor disabled(0x80, 0x80, 0x80), highlight(0x2a, 0x82, 0xda);
    QPalette p(window, window);   // Light, Mid, Dark and Shadow derived from the button colour
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, window);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, disabled);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Link, highlight);
    p.setColor(QPalette::LinkVisited, highlight.darker(120));
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x50, 0x50, 0x50));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    return p;
}

QPalette lightPalette()
{
    // The colours of the Fusion style's light scheme. (Its standardPalette()
    // is not that: it follows the system's scheme since Qt 6.5.)
    const QColor window(0xef, 0xef, 0xef), text(Qt::black), disabled(0xbe, 0xbe, 0xbe);
    const QColor highlight(0x30, 0x8c, 0xc6);
    QPalette p(window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, Qt::white);
    p.setColor(QPalette::AlternateBase, QColor(0xf7, 0xf7, 0xf7));
    p.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xdc));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(0x80, 0x80, 0x80));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Link, Qt::blue);
    p.setColor(QPalette::LinkVisited, Qt::magenta);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x91, 0x9e, 0xa9));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, Qt::white);
    return p;
}

} // namespace qucs_s::apptheme
