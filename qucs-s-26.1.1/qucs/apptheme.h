/*
 * apptheme.h - the application's look: the system's, dark or light
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

#include <QPalette>

/*!
 * The colours of the application's windows, menus, docks and dialogs:
 * what the operating system shows, or dark or light regardless of it.
 * Set in the application settings ("Theme"), applied at start and when
 * the setting changes. Where Qt can ask the platform for a dark or light
 * appearance (macOS, Windows) that is done, so the native controls follow
 * too; elsewhere the application palette is set. The schematic's own
 * colours (document background, grid) are settings of their own.
 */
namespace qucs_s::apptheme {

enum Theme { System = 0, Dark = 1, Light = 2 };

/// A stored value made valid: System when it is none of the three.
int bounded(int theme);

/// Gives the running application the look of \a theme.
void apply(int theme);

/// Whether the application currently shows dark colours: its text
/// lighter than its window background.
bool isDark();

/// The palettes a platform that cannot switch its appearance gets.
QPalette darkPalette();
QPalette lightPalette();

} // namespace qucs_s::apptheme

#endif // APPTHEME_H
