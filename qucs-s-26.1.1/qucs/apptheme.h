/*
 * apptheme.h - the application's look: the system's, dark or light, or
 * one of the designed themes
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef APPTHEME_H
#define APPTHEME_H

#include <QColor>
#include <QIcon>
#include <QList>
#include <QPalette>
#include <QString>

/*!
 * The colours of the application's windows, menus, docks and dialogs.
 *
 * System, Dark and Light are the platform's own look: what the operating
 * system shows, or dark or light regardless of it. Where Qt can ask the
 * platform for a dark or light appearance (macOS, Windows) that is done,
 * so the native controls follow too; elsewhere the application palette is
 * set.
 *
 * The designed themes (Daylight, Nord, Dracula...) look the same on every
 * platform: the Fusion style under a proxy that rounds and flattens its
 * controls, a palette, and a style sheet for the toolbars, docks, tabs,
 * menus, scroll bars and the status bar. Each has a schematic paper and
 * grid of its own, which the schematic takes when its paper follows the
 * theme. The platform's window frames and dialogs follow the theme's
 * darkness.
 *
 * Set in the application settings ("Theme") or View > Theme, applied at
 * start and when the setting changes.
 */
namespace qucs_s::apptheme {

/// The themes, as stored in the settings: never renumber.
enum Theme {
    System = 0,
    Dark = 1,
    Light = 2,
    Daylight = 3,
    Paper = 4,
    SolarizedLight = 5,
    CatppuccinLatte = 6,
    Graphite = 7,
    Nord = 8,
    Dracula = 9,
    OneDark = 10,
    SolarizedDark = 11,
    CatppuccinMocha = 12,
};

/// A designed theme's colours.
struct Colours {
    QColor window;      ///< windows and dialogs
    QColor surface;     ///< toolbars, dock titles, the status bar, the menu bar
    QColor base;        ///< lists, text fields, text areas
    QColor alternate;   ///< every other row of a list
    QColor raised;      ///< buttons, menus, tooltips' frames
    QColor border;      ///< lines between areas, the frames of fields and buttons
    QColor text;
    QColor muted;       ///< secondary text: tabs not shown, the status bar
    QColor disabled;
    QColor accent;      ///< selection, focus, the current tab
    QColor onAccent;    ///< text on the accent
    QColor link;
    QColor paper;       ///< the schematic's paper, when it follows the theme
    QColor grid;        ///< and its grid
};

/// A designed theme.
struct Designed {
    int id;
    const char* name;   ///< untranslated; name() gives it translated
    bool dark;
    Colours colours;
};

/// The designed themes, the light ones first.
const QList<Designed>& designed();
/// The designed theme \a theme is, or nullptr (System, Dark, Light, an
/// unknown value).
const Designed* designedTheme(int theme);
/// Every theme in the order a menu lists them: System, Dark and Light,
/// then the designed ones.
QList<int> themes();
/// A theme's name as shown.
QString name(int theme);

/// A stored value made valid: System when it is no theme.
int bounded(int theme);

/// Gives the running application the look of \a theme.
void apply(int theme);
/// The theme last applied.
int current();
/// Whether a designed theme is on the application.
bool designedInUse();

/// The style the platform's themes draw with ("macOS", "Fusion",
/// "Windows"...): the application's style while one of them is on, the
/// one to go back to while a designed theme is. Setting it changes the
/// style now or when a platform theme comes back.
QString nativeStyle();
void setNativeStyle(const QString& style);

/// Whether the application currently shows dark colours: its text
/// lighter than its window background.
bool isDark();

/// The schematic's paper and grid in \a theme: a designed theme's own,
/// the dark paper in a dark platform theme. Invalid when the theme has
/// none (the light platform themes: the document background setting).
QColor paper(int theme);
QColor grid(int theme);

/// The palettes a platform that cannot switch its appearance gets.
QPalette darkPalette();
QPalette lightPalette();

/// A designed theme's palette and style sheet.
QPalette palette(const Colours& colours);
QString styleSheet(const Colours& colours);

/// A little picture of a theme for its menu entry: the window, the
/// accent and the paper.
QIcon swatch(int theme);

/// The colour a fraction \a t of the way from \a a to \a b.
QColor mix(const QColor& a, const QColor& b, double t);

} // namespace qucs_s::apptheme

#endif // APPTHEME_H
