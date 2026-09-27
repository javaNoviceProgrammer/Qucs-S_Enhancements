/*
 * The settings as a file, to share with others or keep: File > Export
 * Settings writes them, File > Import Settings takes them in.
 *
 * The file is JSON:
 *
 *   {
 *     "Qucs-S settings": 1,                 the format's version
 *     "exported": "2026-09-27T08:50:12Z",
 *     "exported by": "Qucs-S 26.1.3",
 *     "platform": "macOS 26.5 (arm64)",
 *     "settings": { "maxUndo": 20, "ClaudeCode/model": "...", ... }
 *   }
 *
 * "settings" holds the application's store (settings.h) key by key, a
 * group's keys as "Group/key", every value as JSON: text, a number,
 * true/false, a list of text; bytes as {"bytes": base64}, a colour as
 * {"color": "#aarrggbb"}, anything else as {"variant": base64} (the
 * value's QDataStream).
 *
 * What is not a preference but the session's state - where the windows
 * were, the recent files, the last folders, the first start, what the
 * Claude Code program here offers - is neither exported nor touched by an
 * import (isState()).
 */
#ifndef QUCS_SETTINGSIO_H
#define QUCS_SETTINGSIO_H

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace qucs_s::settingsio {

/// The version of the format this build writes and the newest it reads.
constexpr int FormatVersion = 1;

/// The store's key is the session's state, not a preference.
bool isState(const QString& key);

/// The settings as a document (see above).
QJsonObject toJson();
/// Writes them to \a path; false (and why in \a error) when it cannot.
bool exportTo(const QString& path, QString* error = nullptr);

/// What taking a settings file in does here, worked out beforehand.
struct Import {
    QString source;               ///< who made it: "Qucs-S 26.1.3, macOS 26.5 (arm64), 27 Sep 2026 08:50"
    int count = 0;                ///< the settings in the file
    QMap<QString, QVariant> set;  ///< what the store takes
    QStringList remove;           ///< the preferences here the file has not: back to their defaults
    /// The file's settings not taken, each with why: paths not on this
    /// computer, Claude allowed to act without asking, a value not
    /// understood. What this computer has stays.
    QStringList kept;
    /// The programs Qucs-S runs that it changes ("ngspice: /usr/bin/ngspice").
    QStringList programs;
};

/// Reads a settings file (\a path) and works out what importing it does:
/// false, with why in \a error, when it is not one this build reads.
bool read(const QString& path, Import& out, QString* error = nullptr);
bool fromJson(const QJsonObject& doc, Import& out, QString* error = nullptr);

/// Writes \a import into the store. The application reads it again and
/// brings itself in line (QucsApp::importSettingsFrom()).
void apply(const Import& import);

/// Saves the settings as they are into the backup folder before an import
/// replaces them: the file's path, empty (and why in \a error) when it
/// cannot. The last ten are kept.
QString backup(QString* error = nullptr);
/// Where backups go: the application's data folder unless set (the tests,
/// a run with QUCS_SETTINGS_DIR).
QString backupDirectory();
void setBackupDirectory(const QString& dir);

} // namespace qucs_s::settingsio

#endif // QUCS_SETTINGSIO_H
