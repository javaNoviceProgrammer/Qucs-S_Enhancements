/*
 * Keeps a test's settings out of the user's own preferences.
 *
 * The application opens its store through QucsSettingsFile (settings.h),
 * in QSettings::defaultFormat(): once this has been called, the settings
 * manager, the recent-file lists a QucsApp writes as documents open, the
 * dialog geometries, all go to an INI file under the given directory.
 * Call it before the first QucsApp is created.
 */
#ifndef ISOLATED_SETTINGS_H
#define ISOLATED_SETTINGS_H

#include <QSettings>
#include <QString>

inline void useIsolatedSettings(const QString& dir)
{
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir);
}

#endif // ISOLATED_SETTINGS_H
